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
field("bridge").value = params.get("bridge") ?? saved.bridge ?? `ws://${home}:7480`;
field("domain").value = params.get("domain") ?? saved.domain ?? "0";
field("passphrase").value = saved.passphrase ?? "";

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
// over ws:// on port 7480. A LAN name such as "karbon.lan" almost always is too, so it is tried
// there first and then as a public name, a bridge behind a TLS proxy over wss://. A secure page
// may open ws:// only to an IP or a .local name, so there it goes by its .local twin. An address
// with its scheme stays as typed.
function candidates(text) {
    const t = text.trim();
    if (/^wss?:\/\//i.test(t)) return [t];
    const port = t.match(/:(\d+)$/)?.[1];
    const host = port ? t.slice(0, -port.length - 1) : t;
    const ws = (h) => `ws://${h}:${port ?? 7480}`;
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

async function connect(url, passphrase) {
    show("connecting to " + url);
    const net = await TunnelNet.connect(url, {
        passphrase,
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
            net = await connect(url, settings.passphrase);
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

// Every bridge answers rant-bridges.local and lists the others on its network, so that name finds
// them all. Where the name does not resolve, the address typed asks the bridge there.
const findButton = document.getElementById("find");
const found = document.getElementById("found");
findButton.addEventListener("click", async () => {
    findButton.textContent = "Looking for bridges...";
    let list = [];
    for (const url of ["ws://rant-bridges.local:7480", ...candidates(field("bridge").value)]) {
        try { list = await findBridges(url); } catch { continue; }
        if (list.length) break;
    }
    if (!list.length) {
        findButton.textContent = "No bridge answered, type its address";
        return;
    }
    found.replaceChildren(new Option(`${list.length} found, pick one`, ""),
                          ...list.map((b) => new Option(`${b.name}  ${b.url.replace(/^ws:\/\//, "")}`, b.url)));
    findButton.hidden = true;
    found.hidden = false;
});
found.addEventListener("change", () => { if (found.value) field("bridge").value = found.value; });

form.addEventListener("submit", (e) => {
    e.preventDefault();
    start({ bridge: field("bridge").value.trim(), domain: field("domain").value.trim(),
            passphrase: field("passphrase").value });
});

if (saved.passphrase && !params.has("ask")) start({ bridge: field("bridge").value.trim(), domain: field("domain").value, passphrase: saved.passphrase });
else form.hidden = false;
