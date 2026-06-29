# flow_raw_backup — lisensfri native ACAP

Lokal backup av FLOW raw-trajectories, kjørt som en egen app rett på kameraet.
**Ingen lisens. Ringer aldri hjem. Ikke avhengig av CamScripter.** Nettopp det du
vil ha for noe som skal overleve at 4G/VPN faller ut.

Appen poller FLOW lokalt (loopback `127.0.0.1:8088`, med automatisk fallback til
kameraets egne LAN-IP-er), gzip-komprimerer hvert svar og skriver det atomisk til
SD-kortet sammen med en `.meta.json` (sha256, antall byte, sequence_number osv.).

---

## Hva du trenger
Bare **Docker** installert på en PC/Mac/Linux. Selve byggingen skjer inne i Axis
sitt offisielle SDK-image — du trenger ikke installere noe kompilator-verktøy selv.

## Steg 1 — (anbefalt) bekreft kameraets arkitektur
M2035-LE er etter all sannsynlighet `armv7hf` (ARTPEC-7), som er standardvalget.
Vil du være helt sikker, åpne i nettleser (bytt ut IP):

```
http://192.168.14.20/axis-cgi/param.cgi?action=list&group=Properties.System.Architecture
```

- Svar `armv7hf` → bruk standard.
- Svar `aarch64` → bygg med `aarch64` (se under).

## Steg 2 — bygg .eap-fila (én kommando)
Fra mappa `native/`:

```bash
./build.sh             # armv7hf (standard)
# eller
./build.sh aarch64
```

Resultatet havner i `dist/flow_raw_backup_1_0_0_<arch>.eap`.

(Windows uten bash: kjør de to docker-kommandoene i `build.sh` manuelt, eller
bruk Git Bash / WSL.)

## Steg 3 — installer på kameraet
I kameraets webgrensesnitt: **Apps → + Add app → velg .eap-fila → Install**.
Appen er satt til `respawn`, så den **starter automatisk** og starter på nytt ved
omstart/krasj.

(Alternativt fra SDK-containeren: `eap-install.sh <kamera-ip> <passord> install`.)

## Steg 4 — verifiser
Åpne app-loggen i kamera-UI (Apps → flow_raw_backup → app log / "Application log").
Du skal se noe slikt:

```
=== flow-raw-backup 1.0.0 starter ===
output_dir: /var/spool/storage/SD_DISK/flow_raw_backup
SD skrivbar OK (ledig: NNNNN MB)
FLOW innlogging OK via https://127.0.0.1:8088 (api_version 1.0)
Oppdaget: cube 0, analytic 0, N raw-sink(s) [Alle4, ...]
OK 200 cam04/Alle4 (12345 bytes -> 2345, 0 s)
```

Filene legger seg som:

```
/var/spool/storage/SD_DISK/flow_raw_backup/
  <kamera>/sink_<id>_<navn>/<YYYY-MM-DD>/<tidsstempel>_poll.json.gz
  <kamera>/sink_<id>_<navn>/<YYYY-MM-DD>/<tidsstempel>_poll.meta.json
  status.json
```

`status.json` gir et raskt øyeblikksbilde (antall skrevne filer, siste status,
ledig plass). `trajectory_count` er bevisst utelatt i denne native-versjonen for å
slippe å parse fleire-MB-svar i C — `.meta.json` har uansett byte-tall + sha256.

## Innstillinger (valgfritt — appen virker uten)
Ved første start skrives en standard `settings.json` til
`/usr/local/packages/flow_raw_backup/localdata/settings.json`. Standardverdiene
passer cam04 rett ut av boksen (serienummer brukes som mappenavn hvis `camera_name`
er tom, `admin/admin`, poll hvert 120. sekund, gzip på, 14 dagers oppbevaring).

Vil du endre noe (f.eks. `camera_name` per kamera, eller intervall), rediger den
fila og start appen på nytt. Felt:

| Felt | Standard | Forklaring |
|------|----------|-----------|
| `camera_name` | `""` | Mappenavn. Tom = kameraets serienummer brukes. |
| `flow_base_url` | `https://127.0.0.1:8088` | Loopback. Endres normalt ikke. |
| `username`/`password` | `admin`/`admin` | FLOW-innlogging. |
| `poll_interval_seconds` | `120` | Hvor ofte hver sink hentes. |
| `output_dir` | `…/SD_DISK/flow_raw_backup` | Lagringssti på SD. |
| `retention_days` | `14` | Eldre dagsmapper slettes automatisk. |
| `gzip` / `gzip_level` | `true` / `6` | Komprimering. |

---

## Viktige forbehold (ærlig)
1. **SD-skrivetillatelse er det ene som ikke kunne testes på forhånd.** En native
   ACAP kjører som en låst lavprivilegert bruker. Hvis loggen ved oppstart sier
   `ADVARSEL: kan ikke skrive til output_dir`, betyr det at den brukeren ikke får
   skrive direkte til SD-stien, og at vi må legge til Axis sin **AXStorage**-API for
   å få korrekt tilgang. Send meg da loggen, så utvider jeg appen — det er en kjent,
   avgrenset endring.
2. **Arkitektur må stemme** (Steg 1). Feil arkitektur = appen lar seg ikke installere.
3. **libcurl-kallet og selve SDK-byggingen** kjørte ikke i mitt testmiljø (mangler
   nett + Axis-bibliotekene der). All forretningslogikk — JSON-parsing, gzip, sha256,
   atomisk skriving, block_info-parsing, selvoppdaging, hovedløkke — er derimot
   kompilert og kjørt ende-til-ende. Skulle SDK-bygget likevel gi en feilmelding, er
   det nesten alltid en triviell én-linjes greie. Lim inn feilen, så fikser jeg den.

## Native ACAP vs. CamScripter — kort
- **Native ACAP (denne):** ingen lisens, ingen phone-home, mest robust ved nettutfall.
  Koster et byggesteg (Docker) og evt. én runde med AXStorage hvis SD-tilgang krever det.
- **CamScripter-mikroapp:** virker allerede på cam04, men prøvelisensen re-valideres mot
  CamStreamer (krever riktig tid + internett). Engangslisens lagres lokalt og bør kjøre
  offline — få det bekreftet skriftlig før du kjøper seks.

Begge skriver eksakt samme fil- og mappestruktur, så import til Supabase fungerer likt
uansett hvilken du lander på.
