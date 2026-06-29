#define _GNU_SOURCE
/*
 * flow_raw_backup  —  Native ACAP-app (C)
 * ------------------------------------------------------------------
 * Lisensfri, egenstyrt lokal backup av FLOW raw-trajectories.
 * Ingen lisens, ringer aldri hjem, ikke avhengig av CamScripter.
 *
 * Avhengigheter (alle finnes i ACAP Native SDK / på enheten):
 *   - libcurl  (HTTP/HTTPS mot FLOW på loopback)
 *   - zlib     (gzip av responsene)
 *   - POSIX    (atomiske skrivinger til SD)
 * JSON parses av en liten innebygd parser (ingen ekstern avhengighet).
 * SHA256 er en kompakt innebygd implementasjon.
 *
 * Logikken speiler n8n-flowen:
 *   block_info -> api_version ; /users/auth -> token ;
 *   /cubes -> cube_id ; /cubes/{c}/analytics -> analytic_id ;
 *   /cubes/{c}/analytics/{a}/sinks -> sinks + sequence_number ;
 *   filtrer raw_trajectories ; POST .../sinks/data -> lagre svaret.
 *
 * Adressering: loopback (127.0.0.1) først, så kameraets egne LAN-IP-er.
 * Aldri public-IP.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <zlib.h>

#ifndef HOST_TEST
#include <curl/curl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <strings.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_NAME    "flow-raw-backup"
#define APP_VERSION "1.1.5"

/* ============================ logging ============================ */

static void lg(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    time_t t = time(NULL); struct tm g; gmtime_r(&t, &g);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &g);
    fprintf(stderr, "[%s] %s\n", ts, buf);
#ifndef HOST_TEST
    syslog(LOG_INFO, "%s", buf);
#endif
}

/* ====================== minimal JSON parser ===================== */

typedef enum { JT_NULL, JT_BOOL, JT_NUM, JT_STR, JT_ARR, JT_OBJ } jtype;
typedef struct jval jval;
typedef struct { char *key; jval *val; } jmember;
struct jval {
    jtype t;
    int b;
    double num;
    char *numraw;
    char *str;
    struct { jval **items; size_t n; } arr;
    struct { jmember *m; size_t n; } obj;
};

typedef struct { const char *p; const char *end; int ok; } jcur;

static jval *jparse_value(jcur *c);

static void jskip(jcur *c) {
    while (c->p < c->end && (*c->p==' '||*c->p=='\t'||*c->p=='\n'||*c->p=='\r')) c->p++;
}

static jval *jnew(jtype t) { jval *v = calloc(1, sizeof(jval)); if (v) v->t = t; return v; }

void jfree(jval *v) {
    if (!v) return;
    if (v->t == JT_STR) free(v->str);
    else if (v->t == JT_NUM) free(v->numraw);
    else if (v->t == JT_ARR) { for (size_t i=0;i<v->arr.n;i++) jfree(v->arr.items[i]); free(v->arr.items); }
    else if (v->t == JT_OBJ) { for (size_t i=0;i<v->obj.n;i++){ free(v->obj.m[i].key); jfree(v->obj.m[i].val);} free(v->obj.m); }
    free(v);
}

/* decode a JSON string (after opening quote already consumed by caller) */
static char *jparse_string_raw(jcur *c) {
    /* assumes *c->p == '"' */
    c->p++;
    size_t cap = 16, len = 0;
    char *out = malloc(cap);
    if (!out) { c->ok = 0; return NULL; }
    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p;
        if (ch == '\\') {
            c->p++;
            if (c->p >= c->end) break;
            char e = *c->p;
            switch (e) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'u': {
                    if (c->end - c->p < 5) { c->ok=0; free(out); return NULL; }
                    char hex[5]; memcpy(hex, c->p+1, 4); hex[4]=0;
                    unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                    c->p += 4;
                    /* encode BMP code point as UTF-8 */
                    if (cp < 0x80) { if(len+1>=cap){cap*=2;out=realloc(out,cap);} out[len++]=(char)cp; }
                    else if (cp < 0x800) { if(len+2>=cap){cap*=2;out=realloc(out,cap);} out[len++]=(char)(0xC0|(cp>>6)); out[len++]=(char)(0x80|(cp&0x3F)); }
                    else { if(len+3>=cap){cap*=2;out=realloc(out,cap);} out[len++]=(char)(0xE0|(cp>>12)); out[len++]=(char)(0x80|((cp>>6)&0x3F)); out[len++]=(char)(0x80|(cp&0x3F)); }
                    c->p++;
                    continue;
                }
                default: ch = e; break;
            }
            c->p++;
        } else {
            c->p++;
        }
        if (len+1 >= cap) { cap*=2; out=realloc(out,cap); if(!out){c->ok=0;return NULL;} }
        out[len++] = ch;
    }
    if (c->p >= c->end || *c->p != '"') { c->ok=0; free(out); return NULL; }
    c->p++; /* closing quote */
    out[len] = 0;
    return out;
}

static jval *jparse_string(jcur *c) {
    char *s = jparse_string_raw(c);
    if (!s) return NULL;
    jval *v = jnew(JT_STR); if(!v){free(s);return NULL;} v->str = s; return v;
}

static jval *jparse_number(jcur *c) {
    const char *start = c->p;
    if (c->p < c->end && (*c->p=='-'||*c->p=='+')) c->p++;
    while (c->p < c->end && (isdigit((unsigned char)*c->p) || *c->p=='.' || *c->p=='e' || *c->p=='E' || *c->p=='+' || *c->p=='-')) c->p++;
    size_t l = c->p - start;
    jval *v = jnew(JT_NUM); if(!v) return NULL;
    v->numraw = malloc(l+1); if(!v->numraw){jfree(v);return NULL;}
    memcpy(v->numraw, start, l); v->numraw[l]=0;
    v->num = strtod(v->numraw, NULL);
    return v;
}

static jval *jparse_array(jcur *c) {
    c->p++; /* [ */
    jval *v = jnew(JT_ARR); if(!v) return NULL;
    size_t cap = 4;
    v->arr.items = malloc(cap * sizeof(jval*));
    jskip(c);
    if (c->p < c->end && *c->p == ']') { c->p++; return v; }
    while (1) {
        jval *item = jparse_value(c);
        if (!c->ok) { jfree(v); return NULL; }
        if (v->arr.n >= cap) { cap*=2; v->arr.items = realloc(v->arr.items, cap*sizeof(jval*)); }
        v->arr.items[v->arr.n++] = item;
        jskip(c);
        if (c->p < c->end && *c->p == ',') { c->p++; jskip(c); continue; }
        if (c->p < c->end && *c->p == ']') { c->p++; break; }
        c->ok = 0; jfree(v); return NULL;
    }
    return v;
}

static jval *jparse_object(jcur *c) {
    c->p++; /* { */
    jval *v = jnew(JT_OBJ); if(!v) return NULL;
    size_t cap = 4;
    v->obj.m = malloc(cap * sizeof(jmember));
    jskip(c);
    if (c->p < c->end && *c->p == '}') { c->p++; return v; }
    while (1) {
        jskip(c);
        if (c->p >= c->end || *c->p != '"') { c->ok=0; jfree(v); return NULL; }
        char *key = jparse_string_raw(c);
        if (!key) { c->ok=0; jfree(v); return NULL; }
        jskip(c);
        if (c->p >= c->end || *c->p != ':') { c->ok=0; free(key); jfree(v); return NULL; }
        c->p++;
        jval *val = jparse_value(c);
        if (!c->ok) { free(key); jfree(v); return NULL; }
        if (v->obj.n >= cap) { cap*=2; v->obj.m = realloc(v->obj.m, cap*sizeof(jmember)); }
        v->obj.m[v->obj.n].key = key;
        v->obj.m[v->obj.n].val = val;
        v->obj.n++;
        jskip(c);
        if (c->p < c->end && *c->p == ',') { c->p++; continue; }
        if (c->p < c->end && *c->p == '}') { c->p++; break; }
        c->ok = 0; jfree(v); return NULL;
    }
    return v;
}

static jval *jparse_value(jcur *c) {
    jskip(c);
    if (c->p >= c->end) { c->ok=0; return NULL; }
    char ch = *c->p;
    if (ch == '"') return jparse_string(c);
    if (ch == '{') return jparse_object(c);
    if (ch == '[') return jparse_array(c);
    if (ch == '-' || isdigit((unsigned char)ch)) return jparse_number(c);
    if (!strncmp(c->p, "true", 4))  { c->p+=4; jval*v=jnew(JT_BOOL); if(v)v->b=1; return v; }
    if (!strncmp(c->p, "false", 5)) { c->p+=5; jval*v=jnew(JT_BOOL); if(v)v->b=0; return v; }
    if (!strncmp(c->p, "null", 4))  { c->p+=4; return jnew(JT_NULL); }
    c->ok = 0; return NULL;
}

jval *jparse(const char *text, size_t len) {
    jcur c; c.p = text; c.end = text + len; c.ok = 1;
    jval *v = jparse_value(&c);
    if (!c.ok) { jfree(v); return NULL; }
    return v;
}

static jval *jget(jval *o, const char *key) {
    if (!o || o->t != JT_OBJ) return NULL;
    for (size_t i=0;i<o->obj.n;i++) if (!strcmp(o->obj.m[i].key, key)) return o->obj.m[i].val;
    return NULL;
}
static const char *jstr(jval *v) { return (v && v->t==JT_STR) ? v->str : NULL; }
static long long jint(jval *v) {
    if (!v) return 0;
    if (v->t==JT_NUM) return (long long)strtoll(v->numraw, NULL, 10);
    if (v->t==JT_STR) return (long long)strtoll(v->str, NULL, 10);
    return 0;
}

/* ============================= SHA256 =========================== */

typedef struct { uint32_t s[8]; uint64_t len; unsigned char buf[64]; size_t n; } sha256_ctx;
static const uint32_t SHA_K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha256_init(sha256_ctx*c){ c->s[0]=0x6a09e667;c->s[1]=0xbb67ae85;c->s[2]=0x3c6ef372;c->s[3]=0xa54ff53a;c->s[4]=0x510e527f;c->s[5]=0x9b05688c;c->s[6]=0x1f83d9ab;c->s[7]=0x5be0cd19;c->len=0;c->n=0; }
static void sha256_block(sha256_ctx*c,const unsigned char*p){
    uint32_t w[64];
    for(int i=0;i<16;i++) w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|(uint32_t)p[i*4+3];
    for(int i=16;i<64;i++){ uint32_t s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3); uint32_t s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
    uint32_t a=c->s[0],b=c->s[1],cc=c->s[2],d=c->s[3],e=c->s[4],f=c->s[5],g=c->s[6],h=c->s[7];
    for(int i=0;i<64;i++){
        uint32_t S1=ROR(e,6)^ROR(e,11)^ROR(e,25); uint32_t ch=(e&f)^((~e)&g); uint32_t t1=h+S1+ch+SHA_K[i]+w[i];
        uint32_t S0=ROR(a,2)^ROR(a,13)^ROR(a,22); uint32_t mj=(a&b)^(a&cc)^(b&cc); uint32_t t2=S0+mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->s[0]+=a;c->s[1]+=b;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}
static void sha256_update(sha256_ctx*c,const unsigned char*p,size_t n){
    c->len+=n;
    while(n){ size_t k=64-c->n; if(k>n)k=n; memcpy(c->buf+c->n,p,k); c->n+=k; p+=k; n-=k; if(c->n==64){ sha256_block(c,c->buf); c->n=0; } }
}
static void sha256_final(sha256_ctx*c,char*hex){
    uint64_t bits=c->len*8; unsigned char pad=0x80; sha256_update(c,&pad,1);
    unsigned char z=0; while(c->n!=56) sha256_update(c,&z,1);
    unsigned char lb[8]; for(int i=0;i<8;i++) lb[i]=(unsigned char)(bits>>(56-8*i)); sha256_update(c,lb,8);
    for(int i=0;i<8;i++) sprintf(hex+i*8,"%08x",c->s[i]);
}
static void sha256_hex(const unsigned char*data,size_t n,char*hex){ sha256_ctx c; sha256_init(&c); sha256_update(&c,data,n); sha256_final(&c,hex); }

/* ============================ helpers =========================== */

static int gzip_buffer(const unsigned char *in, size_t inlen, unsigned char **out, size_t *outlen, int level) {
    z_stream zs; memset(&zs,0,sizeof(zs));
    if (deflateInit2(&zs, level, Z_DEFLATED, 15+16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return -1;
    uLong cap = deflateBound(&zs, inlen) + 64;
    unsigned char *buf = malloc(cap);
    if (!buf) { deflateEnd(&zs); return -1; }
    zs.next_in = (Bytef*)in; zs.avail_in = inlen;
    zs.next_out = buf; zs.avail_out = cap;
    int r = deflate(&zs, Z_FINISH);
    if (r != Z_STREAM_END) { deflateEnd(&zs); free(buf); return -1; }
    *outlen = cap - zs.avail_out; *out = buf; deflateEnd(&zs); return 0;
}

static int mkpath(const char *path, mode_t mode) {
    char tmp[PATH_MAX]; snprintf(tmp,sizeof(tmp),"%s",path);
    size_t n=strlen(tmp);
    for (size_t i=1;i<n;i++) if (tmp[i]=='/') { tmp[i]=0; if(mkdir(tmp,mode)<0 && errno!=EEXIST) return -1; chmod(tmp,mode); tmp[i]='/'; }
    if (mkdir(tmp,mode)<0 && errno!=EEXIST) return -1;
    chmod(tmp,mode);  /* tving mode uavhengig av umask (ignorer feil paa dirs vi ikke eier) */
    return 0;
}

static int atomic_write(const char *path, const unsigned char *data, size_t len) {
    char tmp[PATH_MAX]; snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    int fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) return -1;
    fchmod(fd, 0644);  /* tving 0644 uavhengig av umask, slik at SSH-bruker kan lese */
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data+off, len-off);
        if (w < 0) { if (errno==EINTR) continue; close(fd); unlink(tmp); return -1; }
        off += (size_t)w;
    }
    if (fsync(fd) < 0) { close(fd); unlink(tmp); return -1; }
    if (close(fd) < 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) < 0) { unlink(tmp); return -1; }
    char dir[PATH_MAX]; snprintf(dir,sizeof(dir),"%s",path);
    char *sl = strrchr(dir,'/'); if (sl) { *sl=0; int dfd=open(dir,O_RDONLY); if(dfd>=0){ fsync(dfd); close(dfd);} }
    return 0;
}

/* engangs-reparasjon: gjoer hele treet lesbart for andre brukere (SSH-pipeline) */
static void repair_perms(const char *path) {
    struct stat st;
    if (lstat(path,&st)!=0) return;
    if (S_ISDIR(st.st_mode)) {
        chmod(path,0755);
        DIR *d=opendir(path); if(!d) return;
        struct dirent *e;
        while ((e=readdir(d))) {
            if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
            char p[PATH_MAX]; snprintf(p,sizeof(p),"%s/%s",path,e->d_name);
            repair_perms(p);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        chmod(path,0644);
    }
}

static long long free_bytes(const char *dir) {
    struct statvfs s; if (statvfs(dir,&s)==0) return (long long)s.f_bavail * (long long)s.f_frsize; return -1;
}

static void kv_get(const char *text, const char *key, char *out, size_t outn) {
    out[0]=0; size_t kl=strlen(key); const char *p=text;
    while ((p = strstr(p, key))) {
        if ((p==text || isspace((unsigned char)p[-1])) && p[kl]=='=') {
            const char *v = p+kl+1;
            if (*v=='"') { v++; const char*e=strchr(v,'"'); if(e){ size_t l=(size_t)(e-v); if(l>=outn)l=outn-1; memcpy(out,v,l); out[l]=0; return; } }
            else { const char*e=v; while(*e && !isspace((unsigned char)*e)) e++; size_t l=(size_t)(e-v); if(l>=outn)l=outn-1; memcpy(out,v,l); out[l]=0; return; }
        }
        p += kl;
    }
}

/* parse block_info (JSON or key=value) into out fields */
static void parse_block_info(const char *body, size_t len,
                             char *api_version, size_t av_n,
                             char *serial, size_t se_n,
                             char *block_state, size_t bs_n) {
    api_version[0]=serial[0]=block_state[0]=0;
    const char *p=body; while (p<body+len && isspace((unsigned char)*p)) p++;
    if (p<body+len && *p=='{') {
        jval *o = jparse(body, len);
        if (o) {
            const char *s;
            if ((s=jstr(jget(o,"api_version")))) snprintf(api_version,av_n,"%s",s);
            if ((s=jstr(jget(o,"flow_serial_number")))) snprintf(serial,se_n,"%s",s);
            if ((s=jstr(jget(o,"block_state")))) snprintf(block_state,bs_n,"%s",s);
            jfree(o);
            return;
        }
    }
    /* fallback: key=value text */
    char tmp[PATH_MAX];
    if (len < sizeof(tmp)) { memcpy(tmp,body,len); tmp[len]=0; }
    else { memcpy(tmp,body,sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0; }
    kv_get(tmp,"api_version",api_version,av_n);
    kv_get(tmp,"flow_serial_number",serial,se_n);
    kv_get(tmp,"block_state",block_state,bs_n);
}

static int sink_is_raw(jval *s) {
    const char *ovt = jstr(jget(s,"output_value_type"));
    const char *oa  = jstr(jget(s,"operator_attribute"));
    const char *nm  = jstr(jget(s,"name"));
    if (ovt && !strcmp(ovt,"raw_trajectories")) return 1;
    if (oa  && !strcmp(oa,"raw_trajectories")) return 1;
    if (nm) { char low[256]; size_t i; for(i=0;nm[i]&&i<255;i++) low[i]=tolower((unsigned char)nm[i]); low[i]=0; if (strstr(low,"raw")) return 1; }
    return 0;
}

static void json_escape(const char *in, char *out, size_t outn) {
    size_t o=0;
    for (size_t i=0; in && in[i] && o+2<outn; i++) {
        unsigned char ch=(unsigned char)in[i];
        if (ch=='"'||ch=='\\') { out[o++]='\\'; out[o++]=(char)ch; }
        else if (ch=='\n'){out[o++]='\\';out[o++]='n';}
        else if (ch<0x20){ if(o+6<outn){o+=snprintf(out+o,outn-o,"\\u%04x",ch);} }
        else out[o++]=(char)ch;
    }
    out[o]=0;
}

/* ===================== application (real) ====================== */
#ifndef UNIT_TEST

static void utc_iso(time_t t, char *o, size_t n){ struct tm g; gmtime_r(&t,&g); strftime(o,n,"%Y-%m-%dT%H:%M:%SZ",&g); }
static void utc_day(time_t t, char *o, size_t n){ struct tm g; gmtime_r(&t,&g); strftime(o,n,"%Y-%m-%d",&g); }
static void utc_stamp(time_t t, char *o, size_t n){ struct tm g; gmtime_r(&t,&g); strftime(o,n,"%Y%m%dT%H%M%SZ",&g); }

/* ----------------------------- config -------------------------- */

#ifndef SETTINGS_PATH
#define SETTINGS_PATH "/usr/local/packages/flow_raw_backup/localdata/settings.json"
#endif

typedef struct {
    char camera_name[64];
    char flow_base_url[256];
    char block_info_path[128];
    char username[128];
    char password[128];
    int  poll_interval_seconds;
    int  request_timeout_seconds;
    char output_dir[256];
    int  retention_days;
    int  gzip;
    int  gzip_level;
} config_t;

static const char *DEFAULT_SETTINGS_JSON =
"{\n"
"  \"camera_name\": \"\",\n"
"  \"flow_base_url\": \"https://127.0.0.1:8088\",\n"
"  \"block_info_path\": \"/block_info\",\n"
"  \"username\": \"admin\",\n"
"  \"password\": \"admin\",\n"
"  \"poll_interval_seconds\": 120,\n"
"  \"request_timeout_seconds\": 30,\n"
"  \"output_dir\": \"/var/spool/storage/SD_DISK/flow_raw_backup\",\n"
"  \"retention_days\": 14,\n"
"  \"gzip\": true,\n"
"  \"gzip_level\": 6\n"
"}\n";

static void config_defaults(config_t *c) {
    snprintf(c->camera_name,sizeof(c->camera_name),"%s","");
    snprintf(c->flow_base_url,sizeof(c->flow_base_url),"%s","https://127.0.0.1:8088");
    snprintf(c->block_info_path,sizeof(c->block_info_path),"%s","/block_info");
    snprintf(c->username,sizeof(c->username),"%s","admin");
    snprintf(c->password,sizeof(c->password),"%s","admin");
    c->poll_interval_seconds = 120;
    c->request_timeout_seconds = 30;
    snprintf(c->output_dir,sizeof(c->output_dir),"%s","/var/spool/storage/SD_DISK/flow_raw_backup");
    c->retention_days = 14;
    c->gzip = 1; c->gzip_level = 6;
}

static char *read_file(const char *path, size_t *outlen) {
    FILE *f = fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); if(n<0){fclose(f);return NULL;} fseek(f,0,SEEK_SET);
    char *b = malloc((size_t)n+1); if(!b){fclose(f);return NULL;}
    size_t r = fread(b,1,(size_t)n,f); fclose(f); b[r]=0; if(outlen)*outlen=r; return b;
}

static void cfg_str(jval *o,const char *k,char *dst,size_t n){ const char*s=jstr(jget(o,k)); if(s) snprintf(dst,n,"%s",s); }
static void cfg_int(jval *o,const char *k,int *dst){ jval*v=jget(o,k); if(v&&v->t==JT_NUM)*dst=(int)v->num; }
static void cfg_bool(jval *o,const char *k,int *dst){ jval*v=jget(o,k); if(v){ if(v->t==JT_BOOL)*dst=v->b; else if(v->t==JT_NUM)*dst=(v->num!=0);} }

static void mkdir_of(const char *path){ char d[PATH_MAX]; snprintf(d,sizeof(d),"%s",path); char*sl=strrchr(d,'/'); if(sl){*sl=0; mkpath(d,0755);} }

static void load_config(config_t *c) {
    config_defaults(c);
    size_t len; char *buf = read_file(SETTINGS_PATH,&len);
    if (!buf) {
        mkdir_of(SETTINGS_PATH);
        if (atomic_write(SETTINGS_PATH,(const unsigned char*)DEFAULT_SETTINGS_JSON,strlen(DEFAULT_SETTINGS_JSON))==0)
            lg("Opprettet standard settings.json: %s", SETTINGS_PATH);
        else
            lg("Kunne ikke skrive settings.json (%s) — bruker innebygde defaults.", SETTINGS_PATH);
        return;
    }
    jval *o = jparse(buf,len);
    if (o) {
        cfg_str(o,"camera_name",c->camera_name,sizeof(c->camera_name));
        cfg_str(o,"flow_base_url",c->flow_base_url,sizeof(c->flow_base_url));
        cfg_str(o,"block_info_path",c->block_info_path,sizeof(c->block_info_path));
        cfg_str(o,"username",c->username,sizeof(c->username));
        cfg_str(o,"password",c->password,sizeof(c->password));
        cfg_int(o,"poll_interval_seconds",&c->poll_interval_seconds);
        cfg_int(o,"request_timeout_seconds",&c->request_timeout_seconds);
        cfg_str(o,"output_dir",c->output_dir,sizeof(c->output_dir));
        cfg_int(o,"retention_days",&c->retention_days);
        cfg_bool(o,"gzip",&c->gzip);
        cfg_int(o,"gzip_level",&c->gzip_level);
        jfree(o);
    } else lg("settings.json kunne ikke parses — bruker defaults.");
    free(buf);
}

/* ------------------------------ HTTP --------------------------- */

typedef struct { long status; unsigned char *body; size_t len; char www_auth[256]; } http_resp;
static void http_free(http_resp *r){ free(r->body); r->body=NULL; r->len=0; }

#ifndef HOST_TEST
struct membuf { unsigned char *d; size_t n; size_t cap; };
static size_t wr_cb(void *ptr,size_t sz,size_t nm,void *ud){
    size_t add=sz*nm; struct membuf*m=ud;
    if(m->n+add+1>m->cap){ size_t nc=m->cap?m->cap:8192; while(nc<m->n+add+1)nc*=2; unsigned char*nd=realloc(m->d,nc); if(!nd)return 0; m->d=nd; m->cap=nc; }
    memcpy(m->d+m->n,ptr,add); m->n+=add; return add;
}
static size_t hd_cb(char *ptr,size_t sz,size_t nm,void *ud){
    size_t len=sz*nm; http_resp*r=ud;
    if(len>17 && strncasecmp(ptr,"WWW-Authenticate:",17)==0){
        size_t cl=len-17; const char*s=ptr+17; while(cl&&(*s==' '||*s=='\t')){s++;cl--;}
        if(cl>=sizeof(r->www_auth))cl=sizeof(r->www_auth)-1; memcpy(r->www_auth,s,cl); r->www_auth[cl]=0;
        char*nl=strpbrk(r->www_auth,"\r\n"); if(nl)*nl=0;
    }
    return len;
}
static int http_request(const char *method,const char *url,const char *const *headers,int nheaders,
                        const char *body,size_t bodylen,int timeout,http_resp *out){
    memset(out,0,sizeof(*out));
    CURL *ch=curl_easy_init(); if(!ch) return -1;
    struct membuf mb={0}; struct curl_slist *hl=NULL;
    for(int i=0;i<nheaders;i++) hl=curl_slist_append(hl,headers[i]);
    curl_easy_setopt(ch,CURLOPT_URL,url);
    curl_easy_setopt(ch,CURLOPT_SSL_VERIFYPEER,0L);
    curl_easy_setopt(ch,CURLOPT_SSL_VERIFYHOST,0L);
    curl_easy_setopt(ch,CURLOPT_TIMEOUT,(long)timeout);
    curl_easy_setopt(ch,CURLOPT_CONNECTTIMEOUT,5L); /* rask feiling hvis FLOW ikke er oppe */
    curl_easy_setopt(ch,CURLOPT_WRITEFUNCTION,wr_cb);
    curl_easy_setopt(ch,CURLOPT_WRITEDATA,&mb);
    curl_easy_setopt(ch,CURLOPT_HEADERFUNCTION,hd_cb);
    curl_easy_setopt(ch,CURLOPT_HEADERDATA,out);
    if(hl) curl_easy_setopt(ch,CURLOPT_HTTPHEADER,hl);
    if(strcmp(method,"POST")==0){ curl_easy_setopt(ch,CURLOPT_POST,1L); curl_easy_setopt(ch,CURLOPT_POSTFIELDS,body?body:""); curl_easy_setopt(ch,CURLOPT_POSTFIELDSIZE,(long)bodylen); }
    else curl_easy_setopt(ch,CURLOPT_HTTPGET,1L);
    CURLcode rc=curl_easy_perform(ch);
    int ret=0;
    if(rc!=CURLE_OK){ lg("HTTP-feil %s %s: %s",method,url,curl_easy_strerror(rc)); ret=-1; free(mb.d); }
    else { long code=0; curl_easy_getinfo(ch,CURLINFO_RESPONSE_CODE,&code); out->status=code; out->body=mb.d; out->len=mb.n; }
    if(hl) curl_slist_free_all(hl);
    curl_easy_cleanup(ch);
    return ret;
}
#else
static int http_request(const char *method,const char *url,const char *const *headers,int nheaders,
                        const char *body,size_t bodylen,int timeout,http_resp *out){
    (void)method;(void)headers;(void)nheaders;(void)body;(void)bodylen;(void)timeout;
    memset(out,0,sizeof(*out));
    const char *r="{}";
    if(strstr(url,"block_info")) r="{\"api_version\":\"1.0\",\"flow_serial_number\":\"TEST\",\"block_state\":\"processing\"}";
    else if(strstr(url,"users/auth")) r="{\"access_tokens\":[\"tok\"]}";
    else if(strstr(url,"/sinks/data")) r="{\"sinks\":[{\"id\":4,\"data\":{\"trajectories\":[]}}]}";
    else if(strstr(url,"/sinks")) r="{\"sequence_number\":1,\"sinks\":[{\"id\":4,\"name\":\"Alle4\",\"output_value_type\":\"raw_trajectories\"}]}";
    else if(strstr(url,"/analytics")) r="{\"analytics\":[{\"id\":0}]}";
    else if(strstr(url,"/cubes")) r="{\"cubes\":[{\"id\":0}]}";
    out->status=200; out->len=strlen(r); out->body=malloc(out->len+1); memcpy(out->body,r,out->len+1);
    return 0;
}
#endif

/* base candidates: configured first, then loopback/LAN, https+http.
 * Kort tilkoblings-timeout gjoer at doede kandidater feiler raskt. */
#ifndef HOST_TEST
static void add_base(char bases[][256], int *n, int max, const char *b){
    if(*n>=max) return;
    for(int i=0;i<*n;i++) if(!strcmp(bases[i],b)) return;
    snprintf(bases[(*n)++],256,"%s",b);
}
static int collect_bases(const config_t *c,char bases[][256],int max){
    int n=0,port=8088;
    const char *pc=strrchr(c->flow_base_url,':'); if(pc&&isdigit((unsigned char)pc[1])) port=atoi(pc+1);
    char tmp[256];
    add_base(bases,&n,max,c->flow_base_url);
    snprintf(tmp,sizeof(tmp),"https://127.0.0.1:%d",port); add_base(bases,&n,max,tmp);
    snprintf(tmp,sizeof(tmp),"http://127.0.0.1:%d",port);  add_base(bases,&n,max,tmp);
    struct ifaddrs *ifa=NULL;
    if(getifaddrs(&ifa)==0){
        for(struct ifaddrs *p=ifa;p&&n<max;p=p->ifa_next){
            if(!p->ifa_addr||p->ifa_addr->sa_family!=AF_INET) continue;
            if(p->ifa_flags & IFF_LOOPBACK) continue;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET,&((struct sockaddr_in*)p->ifa_addr)->sin_addr,ip,sizeof(ip));
            snprintf(tmp,sizeof(tmp),"https://%s:%d",ip,port); add_base(bases,&n,max,tmp);
            snprintf(tmp,sizeof(tmp),"http://%s:%d",ip,port);  add_base(bases,&n,max,tmp);
        }
        freeifaddrs(ifa);
    }
    /* Docker-bro-adressene ble fjernet i v1.1.2: FLOW svarer alltid paa
       127.0.0.1:8088, og bro-IP-ene er rutbare men doede paa 8088, noe som
       ga 30s timeout hver under FLOW-oppstart. */
    return n;
}
#else
static int collect_bases(const config_t *c,char bases[][256],int max){ (void)max; snprintf(bases[0],256,"%s",c->flow_base_url); return 1; }
#endif

/* --------------------------- FLOW session ---------------------- */

typedef struct { char base[256]; char token[2048]; char api_version[64]; char serial[64]; int have; } session_t;

static int flow_login(const config_t *c,session_t *s){
    char bases[32][256]; int nb=collect_bases(c,bases,32);
    s->base[0]=0;
    for(int i=0;i<nb;i++){
        char url[512]; snprintf(url,sizeof(url),"%s%s",bases[i],c->block_info_path);
        http_resp r; if(http_request("GET",url,NULL,0,NULL,0,c->request_timeout_seconds,&r)!=0) continue;
        if(r.status<200||r.status>=300||!r.body){ http_free(&r); continue; }
        char av[64],se[64],bs[64];
        parse_block_info((char*)r.body,r.len,av,sizeof(av),se,sizeof(se),bs,sizeof(bs));
        http_free(&r);
        if(!av[0]) continue;
        snprintf(s->base,sizeof(s->base),"%s",bases[i]);
        snprintf(s->api_version,sizeof(s->api_version),"%s",av);
        snprintf(s->serial,sizeof(s->serial),"%s",se);
        if(bs[0]) lg("block_state: %s",bs);
        if(i>0) lg("Loopback svarte ikke — bruker %s",bases[i]);
        break;
    }
    if(!s->base[0]){ lg("Ingen FLOW-base svarte paa block_info"); return -1; }

    char ueb[128],peb[128]; json_escape(c->username,ueb,sizeof(ueb)); json_escape(c->password,peb,sizeof(peb));
    char body[512]; snprintf(body,sizeof(body),"{\"username\":\"%s\",\"password\":\"%s\"}",ueb,peb);
    char av_hdr[96]; snprintf(av_hdr,sizeof(av_hdr),"Accept-Version: %s",s->api_version);
    const char *hdrs[]={"Content-Type: application/json",av_hdr};
    char url[512]; snprintf(url,sizeof(url),"%s/users/auth",s->base);
    http_resp r; if(http_request("POST",url,hdrs,2,body,strlen(body),c->request_timeout_seconds,&r)!=0) return -1;
    int ok=-1;
    if(r.status>=200&&r.status<300&&r.body){
        jval *o=jparse((char*)r.body,r.len);
        if(o){ jval*at=jget(o,"access_tokens");
            if(at&&at->t==JT_ARR&&at->arr.n>0){ const char*t=jstr(at->arr.items[0]); if(t){ snprintf(s->token,sizeof(s->token),"%s",t); s->have=1; ok=0; } }
            jfree(o); }
    }
    if(ok!=0) lg("FLOW login HTTP %ld",r.status);
    else lg("FLOW innlogging OK via %s (api_version %s)",s->base,s->api_version);
    http_free(&r);
    return ok;
}

static int flow_get(const config_t *c,session_t *s,const char *path,http_resp *out){
    char auth[2100]; snprintf(auth,sizeof(auth),"Authorization: Bearer %s",s->token);
    char av[96]; snprintf(av,sizeof(av),"Accept-Version: %s",s->api_version);
    const char *h[]={auth,av};
    char url[700]; snprintf(url,sizeof(url),"%s%s",s->base,path);
    return http_request("GET",url,h,2,NULL,0,c->request_timeout_seconds,out);
}
static int flow_post(const config_t *c,session_t *s,const char *path,const char *body,http_resp *out){
    char auth[2100]; snprintf(auth,sizeof(auth),"Authorization: Bearer %s",s->token);
    char av[96]; snprintf(av,sizeof(av),"Accept-Version: %s",s->api_version);
    const char *h[]={auth,av,"Content-Type: application/json"};
    char url[700]; snprintf(url,sizeof(url),"%s%s",s->base,path);
    return http_request("POST",url,h,3,body,strlen(body),c->request_timeout_seconds,out);
}

/* ------------------------------ status ------------------------- */

static long g_files_written=0;

static void write_status(const config_t *c,session_t *s,const char *last_err,long last_status,int raw_sinks){
    char iso[32]; utc_iso(time(NULL),iso,sizeof(iso));
    long long fb=free_bytes(c->output_dir);
    char errbuf[256]; json_escape(last_err?last_err:"",errbuf,sizeof(errbuf));
    char buf[1280];
    snprintf(buf,sizeof(buf),
      "{\n"
      "  \"app_name\": \"%s\",\n"
      "  \"app_version\": \"%s\",\n"
      "  \"now_utc\": \"%s\",\n"
      "  \"resolved_base_url\": \"%s\",\n"
      "  \"camera_serial\": \"%s\",\n"
      "  \"poll_interval_seconds\": %d,\n"
      "  \"files_written\": %ld,\n"
      "  \"last_http_status\": %ld,\n"
      "  \"last_cycle_raw_sinks\": %d,\n"
      "  \"last_error\": \"%s\",\n"
      "  \"free_bytes\": %lld\n"
      "}\n",
      APP_NAME,APP_VERSION,iso,s->base,s->serial,c->poll_interval_seconds,
      g_files_written,last_status,raw_sinks,errbuf,fb);
    char p[PATH_MAX]; snprintf(p,sizeof(p),"%s/status.json",c->output_dir);
    atomic_write(p,(unsigned char*)buf,strlen(buf));
}

/* ------------------------------ cycle -------------------------- */

static void do_cycle(const config_t *c,session_t *s){
    char errmsg[200]=""; long last_status=0; int raw_count=0;

    if(!s->have){ if(flow_login(c,s)!=0){ write_status(c,s,"login feilet",0,0); return; } }

    http_resp r;
    if(flow_get(c,s,"/cubes",&r)!=0){ s->have=0; write_status(c,s,"cubes-kall feilet",0,0); return; }
    if(r.status==401){ http_free(&r); s->have=0; write_status(c,s,"401 cubes",401,0); return; }
    long long cube_id=-1;
    { jval*o=jparse((char*)r.body,r.len); if(o){ jval*a=jget(o,"cubes"); if(a&&a->t==JT_ARR&&a->arr.n>0) cube_id=jint(jget(a->arr.items[0],"id")); jfree(o);} }
    http_free(&r);
    if(cube_id<0){ write_status(c,s,"ingen cubes",0,0); return; }

    char path[300];
    snprintf(path,sizeof(path),"/cubes/%lld/analytics",cube_id);
    if(flow_get(c,s,path,&r)!=0){ s->have=0; write_status(c,s,"analytics-kall feilet",0,0); return; }
    long long an_id=-1;
    { jval*o=jparse((char*)r.body,r.len); if(o){ jval*a=jget(o,"analytics"); if(a&&a->t==JT_ARR&&a->arr.n>0) an_id=jint(jget(a->arr.items[0],"id")); jfree(o);} }
    http_free(&r);
    if(an_id<0){ write_status(c,s,"ingen analytics",0,0); return; }

    snprintf(path,sizeof(path),"/cubes/%lld/analytics/%lld/sinks",cube_id,an_id);
    if(flow_get(c,s,path,&r)!=0){ s->have=0; write_status(c,s,"sinks-kall feilet",0,0); return; }
    long long seq=0;
    struct { long long id; char name[128]; } rs[64]; int rn=0;
    { jval*o=jparse((char*)r.body,r.len);
      if(o){ jval*sq=jget(o,"sequence_number"); if(sq) seq=jint(sq);
             jval*sk=jget(o,"sinks");
             if(sk&&sk->t==JT_ARR){ for(size_t i=0;i<sk->arr.n&&rn<64;i++){ jval*one=sk->arr.items[i];
                 if(sink_is_raw(one)){ rs[rn].id=jint(jget(one,"id")); const char*nm=jstr(jget(one,"name")); snprintf(rs[rn].name,sizeof(rs[rn].name),"%s",nm?nm:""); rn++; } } }
             jfree(o); } }
    http_free(&r);
    raw_count=rn;

    char camname[64]; snprintf(camname,sizeof(camname),"%s", c->camera_name[0]?c->camera_name:(s->serial[0]?s->serial:"camera"));
    { char names[512]=""; for(int i=0;i<rn;i++){ strncat(names,rs[i].name,sizeof(names)-strlen(names)-1); if(i<rn-1) strncat(names,", ",sizeof(names)-strlen(names)-1);} 
      lg("Oppdaget: cube %lld, analytic %lld, %d raw-sink(s) [%s]",cube_id,an_id,rn,names); }

    for(int i=0;i<rn;i++){
        time_t t0=time(NULL);
        char body[256]; snprintf(body,sizeof(body),"{\"sequence_number\":\"%lld\",\"sinks\":[{\"id\":%lld}]}",seq,rs[i].id);
        snprintf(path,sizeof(path),"/cubes/%lld/analytics/%lld/sinks/data",cube_id,an_id);
        if(flow_post(c,s,path,body,&r)!=0){ s->have=0; snprintf(errmsg,sizeof(errmsg),"data-kall feilet"); continue; }
        if(r.status==401){ http_free(&r); s->have=0; snprintf(errmsg,sizeof(errmsg),"401 data"); break; }
        last_status=r.status;

        time_t now=time(NULL);
        char day[16],stamp[32],iso[32]; utc_day(now,day,sizeof(day)); utc_stamp(now,stamp,sizeof(stamp)); utc_iso(now,iso,sizeof(iso));
        char safe[160]; { size_t k=0; for(size_t j=0; rs[i].name[j]&&k<sizeof(safe)-1; j++){ char ch=rs[i].name[j]; safe[k++]=((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='.'||ch=='_'||ch=='-')?ch:'_'; } safe[k]=0; }
        char sinkdir[PATH_MAX]; snprintf(sinkdir,sizeof(sinkdir),"%s/%s/sink_%lld_%s/%s",c->output_dir,camname,rs[i].id,safe,day);
        mkpath(sinkdir,0755);

        char datafile[PATH_MAX]; size_t stored=r.len;
        if(c->gzip){
            unsigned char *gz; size_t gzl;
            if(gzip_buffer(r.body,r.len,&gz,&gzl,c->gzip_level)==0){
                snprintf(datafile,sizeof(datafile),"%s/%s_poll.json.gz",sinkdir,stamp);
                atomic_write(datafile,gz,gzl); stored=gzl; free(gz);
            } else { snprintf(datafile,sizeof(datafile),"%s/%s_poll.json",sinkdir,stamp); atomic_write(datafile,r.body,r.len); stored=r.len; }
        } else { snprintf(datafile,sizeof(datafile),"%s/%s_poll.json",sinkdir,stamp); atomic_write(datafile,r.body,r.len); stored=r.len; }

        char sha[65]; sha256_hex(r.body,r.len,sha);
        long elapsed=(long)(time(NULL)-t0);
        char dfbase[200]; { const char*sl=strrchr(datafile,'/'); snprintf(dfbase,sizeof(dfbase),"%s",sl?sl+1:datafile); }
        char nesc[200]; json_escape(rs[i].name,nesc,sizeof(nesc));

        char meta[1700];
        snprintf(meta,sizeof(meta),
          "{\n"
          "  \"app_name\": \"%s\",\n"
          "  \"app_version\": \"%s\",\n"
          "  \"camera_name\": \"%s\",\n"
          "  \"camera_serial\": \"%s\",\n"
          "  \"poll_utc\": \"%s\",\n"
          "  \"flow_base_url\": \"%s\",\n"
          "  \"api_version\": \"%s\",\n"
          "  \"cube_id\": %lld,\n"
          "  \"analytic_id\": %lld,\n"
          "  \"sink_id\": %lld,\n"
          "  \"sink_name\": \"%s\",\n"
          "  \"sequence_number\": \"%lld\",\n"
          "  \"http_status\": %ld,\n"
          "  \"response_sha256\": \"%s\",\n"
          "  \"response_bytes\": %zu,\n"
          "  \"stored_bytes\": %zu,\n"
          "  \"data_file\": \"%s\",\n"
          "  \"elapsed_seconds\": %ld\n"
          "}\n",
          APP_NAME,APP_VERSION,camname,s->serial,iso,s->base,s->api_version,
          cube_id,an_id,rs[i].id,nesc,seq,r.status,sha,r.len,stored,dfbase,elapsed);
        char metafile[PATH_MAX]; snprintf(metafile,sizeof(metafile),"%s/%s_poll.meta.json",sinkdir,stamp);
        atomic_write(metafile,(unsigned char*)meta,strlen(meta));

        g_files_written++;
        lg("OK %ld %s/%s (%zu bytes -> %zu, %ld s)",r.status,camname,rs[i].name,r.len,stored,elapsed);
        http_free(&r);
    }
    write_status(c,s,errmsg,last_status,raw_count);
}

/* ---------------------------- retention ------------------------ */

static void retention_sweep(const config_t *c){
    if(c->retention_days<=0) return;
    time_t cutoff=time(NULL)-(time_t)c->retention_days*86400;
    DIR *d0=opendir(c->output_dir); if(!d0) return;
    struct dirent *e0;
    while((e0=readdir(d0))){
        if(e0->d_name[0]=='.') continue;
        char camdir[PATH_MAX]; snprintf(camdir,sizeof(camdir),"%s/%s",c->output_dir,e0->d_name);
        struct stat st; if(stat(camdir,&st)!=0||!S_ISDIR(st.st_mode)) continue;
        DIR *d1=opendir(camdir); if(!d1) continue;
        struct dirent *e1;
        while((e1=readdir(d1))){
            if(e1->d_name[0]=='.') continue;
            char sinkdir[PATH_MAX]; snprintf(sinkdir,sizeof(sinkdir),"%s/%s",camdir,e1->d_name);
            if(stat(sinkdir,&st)!=0||!S_ISDIR(st.st_mode)) continue;
            DIR *d2=opendir(sinkdir); if(!d2) continue;
            struct dirent *e2;
            while((e2=readdir(d2))){
                if(e2->d_name[0]=='.') continue;
                struct tm tmd; memset(&tmd,0,sizeof(tmd));
                if(!strptime(e2->d_name,"%Y-%m-%d",&tmd)) continue;
                time_t dayt=timegm(&tmd);
                if(dayt>=cutoff) continue;
                char daydir[PATH_MAX]; snprintf(daydir,sizeof(daydir),"%s/%s",sinkdir,e2->d_name);
                DIR *d3=opendir(daydir); if(d3){ struct dirent*e3; while((e3=readdir(d3))){ if(e3->d_name[0]=='.')continue; char f[PATH_MAX]; snprintf(f,sizeof(f),"%s/%s",daydir,e3->d_name); unlink(f);} closedir(d3);} 
                if(rmdir(daydir)==0) lg("Retention: slettet %s",daydir);
            }
            closedir(d2);
        }
        closedir(d1);
    }
    closedir(d0);
}

/* ------------------------------ main --------------------------- */

#ifndef EMERG_DIR
#define EMERG_DIR "/usr/local/packages/flow_raw_backup/localdata/data"
#endif
#ifndef EMERG_CAP_BYTES
#define EMERG_CAP_BYTES (100LL*1024*1024)   /* 100 MB nødbuffer-tak */
#endif
#ifndef EMERG_MIN_FREE
#define EMERG_MIN_FREE  (8LL*1024*1024)      /* hold alltid >=8 MB ledig paa flash */
#endif

/* flytt en fil (samme eller paa tvers av filsystem) */
static int move_file(const char *src, const char *dst){
    if(rename(src,dst)==0) return 0;
    if(errno!=EXDEV) return -1;
    int in=open(src,O_RDONLY); if(in<0) return -1;
    char tmp[PATH_MAX]; snprintf(tmp,sizeof(tmp),"%s.tmp",dst);
    int out=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0644); if(out<0){ close(in); return -1; }
    fchmod(out,0644);
    char b[65536]; ssize_t r; int ok=1;
    while((r=read(in,b,sizeof(b)))>0){ ssize_t off=0; while(off<r){ ssize_t w=write(out,b+off,r-off); if(w<0){ ok=0; break; } off+=w; } if(!ok) break; }
    if(r<0) ok=0;
    fsync(out); close(out); close(in);
    if(!ok){ unlink(tmp); return -1; }
    if(rename(tmp,dst)!=0){ unlink(tmp); return -1; }
    unlink(src);
    return 0;
}

/* flytt alle _poll.*-filer fra nødbuffer til SD, behold mappestruktur */
static int migrate_walk(const char *buf_base, const char *sd_base, const char *rel){
    char dir[PATH_MAX];
    if(rel[0]) snprintf(dir,sizeof(dir),"%s/%s",buf_base,rel); else snprintf(dir,sizeof(dir),"%s",buf_base);
    DIR *d=opendir(dir); if(!d) return 0;
    struct dirent *e; int moved=0;
    while((e=readdir(d))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
        char childrel[PATH_MAX];
        if(rel[0]) snprintf(childrel,sizeof(childrel),"%s/%s",rel,e->d_name); else snprintf(childrel,sizeof(childrel),"%s",e->d_name);
        char full[PATH_MAX]; snprintf(full,sizeof(full),"%s/%s",buf_base,childrel);
        struct stat st; if(stat(full,&st)!=0) continue;
        if(S_ISDIR(st.st_mode)){
            moved += migrate_walk(buf_base,sd_base,childrel);
        } else if(S_ISREG(st.st_mode)){
            if(strstr(e->d_name,"_poll.")){
                char dst[PATH_MAX]; snprintf(dst,sizeof(dst),"%s/%s",sd_base,childrel);
                char dstdir[PATH_MAX]; snprintf(dstdir,sizeof(dstdir),"%s",dst);
                char *sl=strrchr(dstdir,'/'); if(sl){ *sl=0; mkpath(dstdir,0755); }
                if(move_file(full,dst)==0) moved++;
            } else {
                unlink(full);  /* status.json, .wtest osv. */
            }
        }
    }
    closedir(d);
    if(rel[0]) rmdir(dir);  /* fjern naa-tom mappe (ikke selve buffer-roten) */
    return moved;
}
static void migrate_buffer_to_sd(const char *buf_base, const char *sd_base){
    struct stat st; if(stat(buf_base,&st)!=0||!S_ISDIR(st.st_mode)) return;
    int n=migrate_walk(buf_base,sd_base,"");
    if(n>0) lg("Flyttet %d noed-bufrede filer fra intern flash til SD.",n);
}

/* samle filer (for tak-haandhevelse) */
typedef struct { char path[PATH_MAX]; time_t mt; long long sz; } finfo;
static void collect_files(const char *dir, finfo **arr, int *n, int *cap){
    DIR *d=opendir(dir); if(!d) return;
    struct dirent *e;
    while((e=readdir(d))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
        char p[PATH_MAX]; snprintf(p,sizeof(p),"%s/%s",dir,e->d_name);
        struct stat st; if(stat(p,&st)!=0) continue;
        if(S_ISDIR(st.st_mode)) collect_files(p,arr,n,cap);
        else if(S_ISREG(st.st_mode) && strstr(e->d_name,"_poll.")){
            if(*n>=*cap){ *cap=(*cap)?(*cap)*2:128; *arr=realloc(*arr,(*cap)*sizeof(finfo)); }
            snprintf((*arr)[*n].path,PATH_MAX,"%s",p); (*arr)[*n].mt=st.st_mtime; (*arr)[*n].sz=(long long)st.st_size; (*n)++;
        }
    }
    closedir(d);
}
static int cmp_mt(const void*a,const void*b){ const finfo*x=a,*y=b; return (x->mt<y->mt)?-1:(x->mt>y->mt)?1:0; }

/* slett eldste filer til total <= tak OG ledig plass >= margin */
static void enforce_cap(const char *base, long long max_bytes){
    finfo *arr=NULL; int n=0,cap=0;
    collect_files(base,&arr,&n,&cap);
    if(n>1) qsort(arr,n,sizeof(finfo),cmp_mt);  /* eldste foerst */
    long long total=0; for(int i=0;i<n;i++) total+=arr[i].sz;
    int removed=0,i=0;
    while(i<n && (total>max_bytes || free_bytes(base)<EMERG_MIN_FREE)){
        if(unlink(arr[i].path)==0){ total-=arr[i].sz; removed++; }
        i++;
    }
    if(removed>0) lg("Noedbuffer ryddet: slettet %d eldste filer (plass/tak).",removed);
    free(arr);
}

/* Velg en skrivbar SD-sti. Returnerer 1 og setter cfg->output_dir, ellers 0.
 * Skriver ALDRI til intern flash her — kun SD-kortet. */
static int ensure_sd_storage(config_t *cfg){
#ifdef HOST_TEST
    if(getenv("FRB_NO_SD")) return 0;  /* testhjelp: simuler manglende SD */
#endif
    const char *cands[3]; int nc=0;
    if(strstr(cfg->output_dir,"/storage/")) cands[nc++]=cfg->output_dir;  /* konfigurert SD-sti foerst */
    cands[nc++]="/var/spool/storage/areas/SD_DISK/flow_raw_backup";
    cands[nc++]="/var/spool/storage/SD_DISK/flow_raw_backup";
    for(int i=0;i<nc;i++){
        if(!cands[i]||!cands[i][0]) continue;
        if(mkpath(cands[i],0755)!=0) continue;
        char tf[PATH_MAX]; snprintf(tf,sizeof(tf),"%s/.wtest",cands[i]);
        if(atomic_write(tf,(const unsigned char*)"ok",2)==0){
            unlink(tf);
            if(strcmp(cfg->output_dir,cands[i])!=0) snprintf(cfg->output_dir,sizeof(cfg->output_dir),"%s",cands[i]);
            return 1;
        }
    }
    return 0;
}

static volatile sig_atomic_t g_stop=0;
static void on_term(int sig){ (void)sig; g_stop=1; }

int main(void){
#ifndef HOST_TEST
    openlog("flow_raw_backup",LOG_PID,LOG_USER);
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    signal(SIGTERM,on_term);
    signal(SIGINT,on_term);

    config_t cfg; load_config(&cfg);
    umask(022);  /* slik at filer/mapper blir lesbare for SSH-bruker (0644/0755), ikke 0600/0700 */
    lg("=== %s %s starter ===",APP_NAME,APP_VERSION);

#ifndef HOST_TEST
    /* nettverkstest: naar vi kameraets egen webserver paa loopback? */
    {
        http_resp r;
        if(http_request("GET","https://127.0.0.1/axis-cgi/usergroup.cgi",NULL,0,NULL,0,5,&r)==0){
            lg("Nettverk: naadde kameraets webserver paa loopback (HTTP %ld) — nettverkstilgang OK",r.status);
            http_free(&r);
        } else {
            lg("Nettverk: naadde IKKE kameraets egen webserver paa loopback — appen mangler kanskje nettverkstilgang");
        }
    }
#endif

    session_t s; memset(&s,0,sizeof(s));
    time_t last_retention=0;
    char active_dir[256]=""; int warned_no_sd=0;
    while(!g_stop){
        if(ensure_sd_storage(&cfg)){
            warned_no_sd=0;
            if(strcmp(active_dir,cfg.output_dir)!=0){
                /* SD ble (re)etablert som aktiv (oppstart eller etter noed-modus):
                 * flytt evt. gjenliggende noedbuffer fra flash til SD foerst. */
                migrate_buffer_to_sd(EMERG_DIR, cfg.output_dir);
                snprintf(active_dir,sizeof(active_dir),"%s",cfg.output_dir);
                repair_perms(cfg.output_dir);
                long long fb=free_bytes(cfg.output_dir);
                lg("Lagring OK (SD): %s (ledig: %lld MB)",cfg.output_dir,fb>0?fb/1048576:-1);
                lg("Filrettigheter justert for lesetilgang (0644/0755)");
            }
            do_cycle(&cfg,&s);
            time_t now=time(NULL);
            if(cfg.retention_days>0 && now-last_retention>3600){ retention_sweep(&cfg); last_retention=now; }
        } else {
            /* SD utilgjengelig -> skriv til begrenset noedbuffer paa intern flash */
            if(!warned_no_sd){
                lg("ADVARSEL: SD-kort ikke tilgjengelig — bruker noedbuffer paa intern flash (maks 100 MB). Sjekk SD-kortet.");
                warned_no_sd=1;
            }
            active_dir[0]=0;
            snprintf(cfg.output_dir,sizeof(cfg.output_dir),"%s",EMERG_DIR);
            mkpath(cfg.output_dir,0755);
            enforce_cap(EMERG_DIR, EMERG_CAP_BYTES);   /* frigjoer plass foer skriving */
            do_cycle(&cfg,&s);
        }
        int iv=cfg.poll_interval_seconds<15?15:cfg.poll_interval_seconds;
        for(int i=0;i<iv && !g_stop;i++) sleep(1);
    }
    lg("Avslutter.");
#ifndef HOST_TEST
    curl_global_cleanup();
#endif
    return 0;
}

#endif /* !UNIT_TEST */

#ifdef UNIT_TEST
/* ====================== unit tests (host) ====================== */
#include <assert.h>
int main(void) {
    /* SHA256 known vector */
    char hex[65];
    sha256_hex((const unsigned char*)"abc",3,hex);
    printf("sha256(abc)=%s\n", hex);
    assert(!strcmp(hex,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    /* JSON: login */
    jval *o = jparse("{\"access_tokens\":[\"abc.def\"]}", strlen("{\"access_tokens\":[\"abc.def\"]}"));
    assert(o);
    jval *at = jget(o,"access_tokens"); assert(at && at->t==JT_ARR && at->arr.n==1);
    assert(!strcmp(jstr(at->arr.items[0]),"abc.def"));
    jfree(o);

    /* JSON: sinks + sequence_number + raw filter */
    const char *sj = "{\"sequence_number\":12345,\"sinks\":[{\"id\":4,\"name\":\"Alle4\",\"output_value_type\":\"raw_trajectories\"},{\"id\":9,\"name\":\"Counts\",\"output_value_type\":\"counting\"}]}";
    o = jparse(sj, strlen(sj)); assert(o);
    assert(jint(jget(o,"sequence_number"))==12345);
    jval *sinks=jget(o,"sinks"); assert(sinks && sinks->t==JT_ARR && sinks->arr.n==2);
    assert(sink_is_raw(sinks->arr.items[0])==1);
    assert(sink_is_raw(sinks->arr.items[1])==0);
    assert(jint(jget(sinks->arr.items[0],"id"))==4);
    assert(!strcmp(jstr(jget(sinks->arr.items[0],"name")),"Alle4"));
    jfree(o);

    /* JSON: cubes/analytics */
    o=jparse("{\"cubes\":[{\"id\":0,\"name\":\"C\"}]}", strlen("{\"cubes\":[{\"id\":0,\"name\":\"C\"}]}")); assert(o);
    assert(jint(jget(jget(o,"cubes")->arr.items[0],"id"))==0); jfree(o);

    /* block_info JSON */
    char av[64],se[64],bs[64];
    const char *bi="{\"api_version\":\"1.0\",\"flow_serial_number\":\"B8A44FFD7210\",\"block_state\":\"processing\"}";
    parse_block_info(bi,strlen(bi),av,sizeof(av),se,sizeof(se),bs,sizeof(bs));
    assert(!strcmp(av,"1.0")); assert(!strcmp(se,"B8A44FFD7210")); assert(!strcmp(bs,"processing"));

    /* block_info key=value */
    const char *bi2="api_version=1.0 block_state=processing flow_serial_number=B8A44FFD7210";
    parse_block_info(bi2,strlen(bi2),av,sizeof(av),se,sizeof(se),bs,sizeof(bs));
    assert(!strcmp(av,"1.0")); assert(!strcmp(se,"B8A44FFD7210")); assert(!strcmp(bs,"processing"));

    /* unicode escape in name */
    o=jparse("{\"name\":\"S\\u00f8r6\"}", strlen("{\"name\":\"S\\u00f8r6\"}")); assert(o);
    printf("unicode name=%s\n", jstr(jget(o,"name")));
    assert(!strcmp(jstr(jget(o,"name")),"S\xc3\xb8r6")); jfree(o);

    /* gzip round trip */
    const char *txt="{\"sinks\":[{\"id\":4,\"data\":{\"trajectories\":[1,2,3]}}]}";
    unsigned char *gz; size_t gzl;
    assert(gzip_buffer((const unsigned char*)txt,strlen(txt),&gz,&gzl,6)==0);
    printf("gzip: %zu -> %zu bytes\n", strlen(txt), gzl);
    /* verify gzip magic */
    assert(gz[0]==0x1f && gz[1]==0x8b);
    FILE *f=fopen("/tmp/_t.json.gz","wb"); fwrite(gz,1,gzl,f); fclose(f); free(gz);

    /* atomic write */
    assert(mkpath("/tmp/flowtest/cam04/sink_4_Alle4/2026-06-28",0755)==0);
    assert(atomic_write("/tmp/flowtest/cam04/sink_4_Alle4/2026-06-28/x.json",(const unsigned char*)txt,strlen(txt))==0);

    /* json_escape */
    char esc[128]; json_escape("a\"b\\c",esc,sizeof(esc)); printf("escaped=%s\n",esc);
    assert(!strcmp(esc,"a\\\"b\\\\c"));

    printf("ALL TESTS PASSED\n");
    return 0;
}
#endif
