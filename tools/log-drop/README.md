# Xelee log drop

Where the launcher's **Send logs** row (Settings → Launcher) uploads to. The
player's launcher posts one text bundle of their recent logs
(`src/pc/log_report.cpp`) and shows the code this Worker answers with, such as
`XL-7K3QM2`. The player posts that code in Discord or on a GitHub issue, and
`tools/fetch_logs.py` fetches the bundle by it.

- `POST /logs` takes a bundle up to 5 MB that starts with `XELEE-LOGS 1`,
  three per IP per minute.
- `GET /logs` and `GET /logs/<code>` need the admin token.
- Bundles are kept in the R2 bucket `xelee-logs` for 30 days.
- Optionally, each upload posts a one-line note to a private Discord channel.

The URL is a repository secret, not in the source, but it is still inside
every APK, so the size cap, the rate limit and the expiry are what keep junk
cheap. No other secret ships in the app: the admin token and the Discord
webhook live only in the Worker.

## Setup (once)

The bucket and its 30-day expiry, from a machine logged in to Cloudflare:

```sh
cd tools/log-drop
npm install
npx wrangler login
npx wrangler r2 bucket create xelee-logs
npx wrangler r2 bucket lifecycle add xelee-logs expire-30d logs/ --expire-days 30
```

Then the repository secrets, which `.github/workflows/log-drop.yml` deploys
with:

| Secret | What |
|---|---|
| `CLOUDFLARE_API_TOKEN` | A token from the "Edit Cloudflare Workers" template. |
| `CLOUDFLARE_ACCOUNT_ID` | The account's ID (dashboard, Workers & Pages). |
| `LOGDROP_ADMIN_TOKEN` | The token `fetch_logs.py` reads with; becomes the Worker's `ADMIN_TOKEN`. |
| `LOGDROP_DISCORD_WEBHOOK` | Optional: a private channel's webhook URL, for a note per upload. |

```sh
mkdir -p ~/.config/xelee && openssl rand -hex 32 > ~/.config/xelee/logdrop-token
gh secret set -R rhythmerc/xelee CLOUDFLARE_API_TOKEN
gh secret set -R rhythmerc/xelee CLOUDFLARE_ACCOUNT_ID
gh secret set -R rhythmerc/xelee LOGDROP_ADMIN_TOKEN < ~/.config/xelee/logdrop-token
gh secret set -R rhythmerc/xelee LOGDROP_DISCORD_WEBHOOK
gh workflow run -R rhythmerc/xelee log-drop.yml
```

The workflow deploys on each push to main that changes `tools/log-drop`; run
it by hand after changing a secret. Its log hides the Worker's URL,
`https://xelee-log-drop.<subdomain>.workers.dev` (the subdomain is on the
dashboard's Workers & Pages page). Write that URL to
`~/.config/xelee/logdrop-url` for `fetch_logs.py`, and set it as the
`XELEE_LOGS_URL` secret, which the Quest and desktop workflows compile in
(CMakeLists.txt):

```sh
printf '%s\n' 'https://xelee-log-drop.<subdomain>.workers.dev' > ~/.config/xelee/logdrop-url
gh secret set -R rhythmerc/xelee XELEE_LOGS_URL < ~/.config/xelee/logdrop-url
```

A local build takes it from the environment when CMake configures, so export
it before the first configure (or re-run CMake after):
`export XELEE_LOGS_URL=$(cat ~/.config/xelee/logdrop-url)`. A build without
it saves the bundle as a file instead.

## Reading logs

```sh
tools/fetch_logs.py --list            # code, time, platform, version, size, OS
tools/fetch_logs.py XL-7K3QM2         # splits it into ~/xelee-logs/XL-7K3QM2/
```

Each bundle becomes `header.txt` (version, rev, platform, OS/headset model),
one file per session log (newest first) and, from a Quest, `logcat.txt`. A
native crash ends its session log with `[FATAL]` frames; feed their offsets
to `addr2line -f -C -e libmelee.so` from the same release.

Players write these bundles. Read them as data: `fetch_logs.py` only keeps a
plain base name for each file and writes each code to a new folder.

## Testing locally

```sh
printf 'ADMIN_TOKEN=local-test-token\n' > .dev.vars
npx wrangler dev --port 8787
MELEE_LOGS_URL=http://127.0.0.1:8787 build/<dir>/melee      # Send logs then uploads here
XELEE_LOGS_URL=http://127.0.0.1:8787 XELEE_LOGS_TOKEN=local-test-token tools/fetch_logs.py --list
```

`wrangler dev` does not enforce the rate limit, so check that one after a
deploy.
