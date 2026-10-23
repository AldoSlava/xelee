// The Xelee log drop: the launcher's Send logs row (src/pc/log_report.cpp)
// posts a bundle here and shows the player the code it gets back. Reads need
// ADMIN_TOKEN; tools/fetch_logs.py is the client for them.
//
//   POST /logs             a bundle (text, "XELEE-LOGS 1" first) -> {"code": "XL-7K3QM2"}
//   GET  /logs             recent uploads, newest first (admin)
//   GET  /logs/XL-7K3QM2   one bundle (admin)

const MAX_BYTES = 5 * 1024 * 1024;
const MAGIC = "XELEE-LOGS 1\n";
// Crockford base32: no I, L, O or U, so a code read off a headset and typed
// into Discord comes through.
const ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const CODE = /^XL-[0-9A-HJKMNP-TV-Z]{6}$/;

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname === "/logs" && request.method === "POST") {
      return upload(request, env, ctx);
    }
    if (url.pathname === "/logs" && request.method === "GET") {
      return admin(request, env, () => list(env));
    }
    const code = url.pathname.startsWith("/logs/") ? url.pathname.slice(6) : "";
    if (CODE.test(code) && request.method === "GET") {
      return admin(request, env, () => fetchOne(code, env));
    }
    return json({ error: "Not found" }, 404);
  },
};

async function upload(request, env, ctx) {
  if (env.UPLOADS) {
    const ip = request.headers.get("CF-Connecting-IP") || "local";
    const { success } = await env.UPLOADS.limit({ key: ip });
    if (!success) {
      return json({ error: "Too many uploads; try again in a minute" }, 429);
    }
  }
  const declared = Number(request.headers.get("Content-Length") || 0);
  if (declared > MAX_BYTES) {
    return json({ error: "The logs are too large to send" }, 413);
  }
  const body = new Uint8Array(await request.arrayBuffer());
  if (body.length > MAX_BYTES) {
    return json({ error: "The logs are too large to send" }, 413);
  }
  const head = new TextDecoder().decode(body.subarray(0, 2048));
  if (!head.startsWith(MAGIC)) {
    return json({ error: "Not a Xelee log bundle" }, 400);
  }
  const meta = headerFields(head);
  let code = "";
  for (let i = 0; i < 5 && !code; i++) {
    const candidate = newCode();
    if (!(await env.LOGS.head(key(candidate)))) {
      code = candidate;
    }
  }
  if (!code) {
    return json({ error: "The log server is busy; try again" }, 503);
  }
  await env.LOGS.put(key(code), body, {
    httpMetadata: { contentType: "text/plain; charset=utf-8" },
    customMetadata: meta,
  });
  if (env.DISCORD_WEBHOOK) {
    const kb = Math.ceil(body.length / 1024);
    const line = `New logs **${code}** · ${meta.version || "?"} · ${meta.platform || "?"} · ${kb} KB`;
    ctx.waitUntil(
      fetch(env.DISCORD_WEBHOOK, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ content: line, allowed_mentions: { parse: [] } }),
      }).catch(() => {}),
    );
  }
  return json({ code });
}

async function list(env) {
  const items = [];
  let cursor;
  do {
    const page = await env.LOGS.list({ prefix: "logs/", cursor, include: ["customMetadata"] });
    for (const o of page.objects) {
      items.push({
        code: o.key.slice(5, -4),
        uploaded: o.uploaded.toISOString(),
        size: o.size,
        ...o.customMetadata,
      });
    }
    cursor = page.truncated ? page.cursor : undefined;
  } while (cursor);
  items.sort((a, b) => (a.uploaded < b.uploaded ? 1 : -1));
  return json({ logs: items });
}

async function fetchOne(code, env) {
  const o = await env.LOGS.get(key(code));
  if (!o) {
    return json({ error: "No logs with that code (they are kept 30 days)" }, 404);
  }
  return new Response(o.body, { headers: { "Content-Type": "text/plain; charset=utf-8" } });
}

async function admin(request, env, handler) {
  const want = env.ADMIN_TOKEN || "";
  const got = (request.headers.get("Authorization") || "").replace(/^Bearer /, "");
  if (!want || !(await sameSecret(got, want))) {
    return json({ error: "Not authorized" }, 401);
  }
  return handler();
}

// Compare digests, so the time taken says nothing about the token.
async function sameSecret(a, b) {
  const enc = new TextEncoder();
  const [x, y] = await Promise.all([
    crypto.subtle.digest("SHA-256", enc.encode(a)),
    crypto.subtle.digest("SHA-256", enc.encode(b)),
  ]);
  return crypto.subtle.timingSafeEqual(x, y);
}

// version, rev, platform and os from the bundle's header, for the listing
// and the Discord note. Player-written: kept short and only these keys.
function headerFields(head) {
  const meta = {};
  for (const line of head.split("\n").slice(1, 12)) {
    const m = line.match(/^(version|rev|platform|os): (.{0,120})$/);
    if (m) {
      meta[m[1]] = m[2].replace(/[`*_~|<>@]/g, "");
    }
  }
  return meta;
}

function newCode() {
  const bytes = crypto.getRandomValues(new Uint8Array(6));
  return "XL-" + Array.from(bytes, (b) => ALPHABET[b & 31]).join("");
}

function key(code) {
  return `logs/${code}.txt`;
}

function json(value, status = 200) {
  return new Response(JSON.stringify(value), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}
