// The page around the web explorer. It opens a tunnel to a Rant bridge, then starts the wasm
// build, whose Rant node runs on the sockets the bridge lends.

import { TunnelNet } from "./rant/tunnel.js";
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

field("bridge").value = params.get("bridge") ?? saved.bridge ?? `ws://${location.hostname || "127.0.0.1"}:7480`;
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

async function connect(settings) {
    show("connecting to " + settings.bridge);
    const net = await TunnelNet.connect(settings.bridge, {
        passphrase: settings.passphrase,
        name,
        address: iface === "127.0.0.1" ? iface : "0.0.0.0",
    });
    net.onConnectionChange((c) => show(c.state === "open" ? "" : c.state === "closed" ? "the bridge closed the connection" : c.state + "...", c.state === "closed"));
    return net;
}

async function start(settings) {
    let net;
    try {
        net = await connect(settings);
    } catch (e) {
        show(e?.message ?? String(e), true);
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

form.addEventListener("submit", (e) => {
    e.preventDefault();
    start({ bridge: field("bridge").value.trim(), domain: field("domain").value.trim(),
            passphrase: field("passphrase").value });
});

if (saved.passphrase && !params.has("ask")) start({ bridge: field("bridge").value, domain: field("domain").value, passphrase: saved.passphrase });
else form.hidden = false;
