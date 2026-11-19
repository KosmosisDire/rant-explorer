// The page around the web explorer. It opens a tunnel to a Rant bridge, then starts the wasm
// build, whose Rant node runs on the sockets the bridge lends.

import { TunnelNet, findBridges } from "./rant/tunnel.js";
import createExplorer from "./rant_explorer.js";

// The wasm downloads and compiles while the bridge connects, so the two waits overlap. A
// server that sends it without its wasm type gets the slower compile from bytes.
const wasmUrl = new URL("rant_explorer.wasm", import.meta.url);
const wasm = WebAssembly.compileStreaming(fetch(wasmUrl))
    .catch(() => fetch(wasmUrl).then((r) => r.arrayBuffer()).then((b) => WebAssembly.compile(b)));

/* ---- the page ---- */

const form = document.getElementById("connect");
const field = (name) => form.elements.namedItem(name);
const state = document.getElementById("state");
const params = new URLSearchParams(location.search);
let saved = {};
try { saved = JSON.parse(localStorage.getItem("rant-explorer") ?? "{}"); } catch { /* none kept */ }

// A page the bridge's machine serves points at that machine. A hosted one has no bridge of its
// own, so it starts at this computer's.
const home = location.protocol === "https:" ? "localhost" : location.hostname || "127.0.0.1";
field("bridge").value = params.get("bridge") ?? saved.bridge ?? "";
field("domain").value = params.get("domain") ?? saved.domain ?? "";

function show(text, bad = false) {
    state.textContent = text;
    state.classList.toggle("bad", bad);
    state.hidden = !text;
}

const name = params.get("name") ?? "explorer-web";
// A node pinned to an interface, 127.0.0.1 for one on the bridge's loopback.
const iface = params.get("if");

const IP = /^\d+\.\d+\.\d+\.\d+$/;
const LAN_NAME = /^([^.]+|.+\.(lan|home|internal|localdomain|home\.arpa))$/i;

// Where a typed address may be, tried in order. An IP or a .local name is a bridge on the LAN,
// over ws:// on port 6731. A LAN name such as "karbon.lan" almost always is too, so it is tried
// there first and then as a public name, a bridge behind a TLS proxy over wss://. A secure page
// may open ws:// only to an IP or a .local name, so there it goes by its .local twin. An address
// with its scheme stays as typed.
function candidates(text) {
    const t = text.trim();
    if (/^wss?:\/\//i.test(t)) return [t];
    const port = t.match(/:(\d+)$/)?.[1];
    const host = port ? t.slice(0, -port.length - 1) : t;
    const ws = (h) => `ws://${h}:${port ?? 6731}`;
    const wss = (h) => `wss://${h}${port ? ":" + port : ""}`;
    if (IP.test(host) || /^localhost$|\.local$/i.test(host)) return [ws(host)];
    if (LAN_NAME.test(host)) return [ws(location.protocol === "https:" ? host.split(".")[0] + ".local" : host), wss(host)];
    return [wss(host)];
}

// Why a bridge was never reached, which the browser's own message hides. One that answered said
// what was wrong itself.
function unreached(url, e) {
    if (!/^cannot reach/.test(e?.message ?? "")) return "";
    let why = ". Check that it listens on the network (bind: 0.0.0.0 in its bridge.yaml, then rant bridge restart) and that its firewall lets the port in.";
    if (location.protocol === "https:" && url.startsWith("ws:"))
        why += " A secure page also needs the browser's leave to reach this network: Chrome asks for it, other browsers may refuse.";
    return why;
}

async function connect(url) {
    show("connecting to " + url);
    const net = await TunnelNet.connect(url, {
        name,
        address: iface === "127.0.0.1" ? iface : "0.0.0.0",
    });
    net.onConnectionChange((c) => show(c.state === "open" ? "" : c.state === "closed" ? "the bridge closed the connection" : c.state + "...", c.state === "closed"));
    return net;
}

async function start(settings) {
    // The next place is tried only when this one was never reached: a bridge that answered and
    // refused was the one meant.
    let net = null, first = null;
    for (const url of candidates(settings.bridge)) {
        try {
            net = await connect(url);
            settings = { ...settings, bridge: url };
            break;
        } catch (e) {
            first ??= { url, e };
            if (!/^cannot reach/.test(e?.message ?? "")) { first = { url, e }; break; }
        }
    }
    if (!net) {
        show((first.e?.message ?? String(first.e)) + unreached(first.url, first.e), true);
        form.hidden = false;
        if (!scanning) scan();     /* a kept bridge that failed: offer the ones found instead */
        return;
    }
    try { localStorage.setItem("rant-explorer", JSON.stringify(settings)); } catch { /* not kept */ }
    // A reload connects to what was just chosen, not to the form or a bridge the address named before.
    const address = new URL(location.href);
    address.searchParams.delete("ask");
    if (address.searchParams.has("bridge")) address.searchParams.set("bridge", settings.bridge);
    if (address.searchParams.has("domain")) address.searchParams.set("domain", settings.domain);
    history.replaceState(null, "", address);
    form.hidden = true;
    show("");
    const canvas = document.getElementById("canvas");
    await createExplorer({
        canvas,
        rantNet: net,
        arguments: ["--domain", String(Number(settings.domain) || 0), "--name", name, ...(iface ? ["--if", iface] : [])],
        print: (t) => console.log(t),
        printErr: (t) => console.warn(t),
        instantiateWasm: (imports, done) => {
            wasm.then((m) => WebAssembly.instantiate(m, imports).then((i) => done(i, m)))
                .catch((e) => show(e?.message ?? String(e), true));
            return {};
        },
    });
    canvas.focus();
}

// The explorer's plug button: the page starts over at the form, filled with what it is on now.
globalThis.rantSwitch = () => {
    const address = new URL(location.href);
    address.searchParams.set("ask", "");
    location.assign(address);
};

// The explorer moved to another domain itself, so a reload comes back to it.
globalThis.rantDomain = (domain) => {
    try {
        const kept = JSON.parse(localStorage.getItem("rant-explorer") ?? "{}");
        localStorage.setItem("rant-explorer", JSON.stringify({ ...kept, domain: String(domain) }));
    } catch { /* not kept */ }
    const address = new URL(location.href);
    if (address.searchParams.has("domain")) address.searchParams.set("domain", String(domain));
    history.replaceState(null, "", address);
};

/* ---- the connect form: the bridges found, each with the domains its network has ---- */

const note = document.getElementById("found-note");
const rows = document.getElementById("bridges");
const cols = document.getElementById("cols");
const manual = document.getElementById("manual");
const go = document.getElementById("go");
let found = [];
let scanning = 0;

const plus = () => {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.innerHTML = '<path d="M5 12h14"/><path d="M12 5v14"/>';
    return svg;
};
const validDomain = (text) => /^\d+$/.test(text) && Number(text) <= 65535;

// The fields hold the choice, which a pill writes and a typed address replaces.
function choose(url, domain) {
    field("bridge").value = url;
    field("domain").value = String(domain);
    render();
}

function chosen() {
    const url = field("bridge").value.trim(), domain = field("domain").value.trim();
    if (!url || !validDomain(domain)) return null;
    return { url, domain, bridge: found.find((b) => b.url === url) };
}

function update() {
    const pick = chosen();
    go.disabled = !pick;
    go.textContent = !pick ? "Choose a bridge and domain"
        : `Connect to ${pick.bridge?.name ?? pick.url.replace(/^wss?:\/\//, "")}, domain ${pick.domain}`;
    for (const pill of rows.querySelectorAll(".pill[data-domain]"))
        pill.classList.toggle("on", !!pick && pill.dataset.url === pick.url && pill.dataset.domain === pick.domain);
}

function render() {
    rows.replaceChildren(...found.map((b) => {
        const row = document.createElement("div");
        row.className = "bridge";
        const who = document.createElement("div");
        who.className = "who";
        const name = document.createElement("div");
        name.className = "name";
        name.textContent = b.name;
        const addr = document.createElement("div");
        addr.className = "addr";
        addr.textContent = b.url.replace(/^wss?:\/\//, "");
        who.append(name, addr);
        const domains = document.createElement("div");
        domains.className = "domains";
        if (b.domains && !b.domains.length) {
            const none = document.createElement("span");
            none.className = "none";
            none.textContent = "no nodes";
            domains.append(none);
        }
        for (const d of b.domains ?? []) {
            const pill = document.createElement("button");
            pill.type = "button";
            pill.className = "pill";
            pill.textContent = String(d.domain);
            pill.title = `${d.nodes} ${d.nodes === 1 ? "node" : "nodes"}`;
            pill.dataset.url = b.url;
            pill.dataset.domain = String(d.domain);
            pill.addEventListener("click", () => choose(b.url, d.domain));
            domains.append(pill);
        }
        // Another domain, typed in place of the plus.
        const add = document.createElement("button");
        add.type = "button";
        add.className = "pill add";
        add.title = "Another domain";
        add.append(plus());
        add.addEventListener("click", () => {
            const input = document.createElement("input");
            input.inputMode = "numeric";
            input.placeholder = "0";
            const typed = document.createElement("span");
            typed.className = "pill on";
            typed.append(input);
            add.replaceWith(typed);
            input.focus();
            input.addEventListener("keydown", (e) => {
                if (e.key === "Enter") { e.preventDefault(); input.blur(); }
                if (e.key === "Escape") { input.value = ""; input.blur(); }
            });
            input.addEventListener("blur", () => {
                if (validDomain(input.value.trim())) choose(b.url, Number(input.value.trim()));
                else render();
            });
        });
        domains.append(add);
        row.append(who, domains);
        return row;
    }));
    cols.hidden = !found.length;
    update();
}

// rant-bridges.local reaches whichever bridge answers first, which lists the network's. The
// bridge typed or kept and this computer's are asked too, since either may be elsewhere.
async function scan() {
    const mine = ++scanning;
    note.textContent = "Looking for bridges...";
    const typed = field("bridge").value.trim();
    const urls = ["ws://rant-bridges.local:6731", ...(typed ? candidates(typed) : []), `ws://${home}:6731`];
    const lists = await Promise.all([...new Set(urls)].map((url) => findBridges(url).catch(() => [])));
    if (mine !== scanning) return;
    const byUrl = new Map();
    for (const b of lists.flat())
        if (b.url && (!byUrl.has(b.url) || (b.domains && !byUrl.get(b.url).domains))) byUrl.set(b.url, b);
    found = [...byUrl.values()];
    note.textContent = !found.length ? "No bridge answered. Enter its address below."
        : `Found ${found.length} ${found.length === 1 ? "bridge" : "bridges"}. Pick a domain to join.`;
    if (!found.length) manual.hidden = false;
    render();
}

document.getElementById("rescan").addEventListener("click", scan);
document.getElementById("manual-toggle").addEventListener("click", () => {
    manual.hidden = !manual.hidden;
    if (!manual.hidden) field("bridge").focus();
});
for (const name of ["bridge", "domain"]) field(name).addEventListener("input", update);

form.addEventListener("submit", (e) => {
    e.preventDefault();
    const pick = chosen();
    if (pick) start({ bridge: pick.url, domain: pick.domain });
});

// A bridge that took the page before is connected again at once.
if (saved.bridge && !params.has("ask")) start({ bridge: field("bridge").value.trim(), domain: field("domain").value });
else { form.hidden = false; scan(); }
