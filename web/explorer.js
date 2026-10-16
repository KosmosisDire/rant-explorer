// The page around the web explorer. It opens the node through the bridge client, then starts
// the wasm build, whose node (src/bridge.cpp) runs every entity through window.rantWeb.

import { RantNode } from "./rant-client/rant.js";
import createExplorer from "./rant_explorer.js";

const LEVELS = { error: 0, warn: 1, info: 2 };
const now = () => performance.now() * 1000;   // monotonic microseconds for arrival times
const empty = new Uint8Array(0);

// A schema block as the C++ side reads it: the text is enough, Rant compiles it there.
const block = (b) => (b ? { name: b.name, hash: b.hash, dsl: b.dsl } : null);
const entity = (e) => ({ ...e, schema: block(e.schema), rsp: block(e.rsp), prg: block(e.prg) });

class WebBridge {
    constructor(node) {
        this.node = node;
        this.sink = null;           // set by the module, see js_attach
        this.handles = new Map();   // the module's handle id to its entry
        this.calls = new Map();     // the module's call id to a running task call
        this.epoch = -1;
        this.meshAt = 0;
        this.peersAt = 0;
    }

    // The module is up and listening.
    attached() {
        this.node.onEvent((e) => {
            if (e.kind === "error" || e.kind === "send_error") console.warn("rant:", e.text);
            this.sink.event(`${e.kind}: ${e.text}`);
        });
        this.node.onLog((line) => this.sink.log(LEVELS[line.level] ?? 2, line.node, line.wallUs, line.text))
            .catch((e) => console.warn("rant: logs", e));
        this.reflect();
    }

    // The mesh, folded and per node, read again whenever the bridge's epoch moves, and the
    // peers with their links a few times a second.
    async reflect() {
        for (;;) {
            try {
                if (this.node.connection.state === "open") await this.readMesh();
            } catch (e) {
                console.warn("rant: reflection", e);
            }
            await new Promise((r) => setTimeout(r, 250));
        }
    }

    async readMesh() {
        const r = this.node.reflection;
        const t = performance.now();
        if (t - this.peersAt >= 500) {
            this.peersAt = t;
            this.peers = await r.peers();
            this.sink.peers(JSON.stringify(this.peers));
        }
        const epoch = await r.epoch();
        if (epoch === this.epoch && t - this.meshAt < 5000) return;
        this.epoch = epoch;
        this.meshAt = t;
        const peers = {};
        await Promise.all((this.peers ?? []).filter((p) => p.active).map(async (p) => {
            peers[p.id] = (await r.entities(p.id)).map(entity);
        }));
        const mesh = (await r.mesh()).map(entity);
        this.sink.entities(JSON.stringify({ mesh, peers }));
    }

    open(id, kind, name, spec) {
        const opts = { reliable: spec.reliable, catchUp: spec.catchUp, reflect: spec.reflect,
                       readOnly: spec.readOnly, raw: spec.raw };
        const schema = spec.schema ?? null;
        const n = this.node;
        const deliver = (data, peer) => this.sink.message(id, data, peer, now());
        let made;
        switch (kind) {
        case "subscriber": made = n.subscriber(name, schema, (_, msg) => deliver(msg.data, msg.publisher), opts); break;
        case "publisher": made = n.publisher(name, schema, opts); break;
        case "remote_variable":
            made = n.remoteVariable(name, schema, opts).then((v) => {
                v.onChange((value, info) => deliver(value, info.source));
                return v;
            });
            break;
        case "remote_function": made = n.remoteFunction(name, null, null, opts); break;
        case "remote_task": made = n.remoteTask(name, null, null, null, opts); break;
        default: made = Promise.reject(new Error(`no such kind ${kind}`));
        }
        const entry = { kind, handle: null, closed: false };
        entry.ready = made.then((h) => {
            if (entry.closed) { h.close(); return null; }
            // A bridge that cannot keep typed values as wire bytes sends CBOR, which the
            // module cannot read, so such a handle is refused rather than misread.
            if (spec.raw && !h.raw) {
                h.close();
                throw new Error("this bridge sends typed values as CBOR only, update it to write and call");
            }
            entry.handle = h;
            this.sink.ready(id, JSON.stringify(this.schemas(h)));
            return h;
        }).catch((e) => {
            this.sink.failed(id, `${kind} ${name}: ${e?.message ?? e}`);
            return null;
        });
        this.handles.set(id, entry);
    }

    schemas(h) {
        return { schema: block(h.value?.block), req: block(h.req?.block), rsp: block(h.rsp?.block),
                 prg: block(h.prg?.block) };
    }

    close(id) {
        const entry = this.handles.get(id);
        if (!entry) return;
        this.handles.delete(id);
        entry.closed = true;
        entry.ready.then((h) => h?.close());
    }

    refresh(id) {
        this.handles.get(id)?.ready.then(async (h) => {
            if (!h) return;
            await h.refresh();
            this.sink.ready(id, JSON.stringify(this.schemas(h)));
        }).catch((e) => console.warn("rant: refresh", e));
    }

    matches(id) {
        return this.handles.get(id)?.handle?.matchCount ?? 0;
    }

    send(id, bytes) {
        const entry = this.handles.get(id);
        entry?.ready.then((h) => {
            if (!h) return;
            if (entry.kind === "publisher") h.send(bytes); else h.set(bytes);
        }).catch((e) => console.warn("rant: send", e));
    }

    call(id, call, bytes) {
        const entry = this.handles.get(id);
        const settle = (r) => this.sink.result(call, r.status, r.data ?? empty, r.message ?? "");
        if (!entry) { settle({ status: "cancelled", message: "the handle is closed" }); return; }
        entry.ready.then((h) => {
            if (!h) { settle({ status: "cancelled", message: "the handle did not open" }); return; }
            if (entry.kind !== "remote_task") { h.call(bytes).then(settle); return; }
            const run = h.call(bytes);
            this.calls.set(call, run);
            run.onProgress((value) => this.sink.progress(call, value ?? empty, now()));
            run.result.then((r) => { this.calls.delete(call); settle(r); });
        });
    }

    cancel(call) {
        this.calls.get(call)?.cancel().catch((e) => console.warn("rant: cancel", e));
    }

    meta(req, peer, sections) {
        this.node.reflection.meta(peer, sections)
            .then((r) => this.sink.meta(req, JSON.stringify(r)))
            .catch(() => this.sink.meta(req, JSON.stringify({ valid: false, status: "cancelled", provider: 0, info: {} })));
    }
}

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

async function connect(settings) {
    show("connecting to " + settings.bridge);
    const node = await RantNode.connect(settings.bridge, {
        passphrase: settings.passphrase,
        name: params.get("name") ?? "explorer-web",
        domain: Number(settings.domain) || 0,
        // the explorer watches every name on the mesh, so it needs every peer's schemas
        fetchDetails: true,
        recvBufferBytes: 8 << 20,
        matchWaitMs: -1,
    });
    node.onConnectionChange((c) => show(c.state === "open" ? "" : c.state === "closed" ? "the bridge closed the connection" : c.state + "...", c.state === "closed"));
    return node;
}

async function start(settings) {
    let node;
    try {
        node = await connect(settings);
    } catch (e) {
        show(e?.message ?? String(e), true);
        form.hidden = false;
        return;
    }
    try { localStorage.setItem("rant-explorer", JSON.stringify(settings)); } catch { /* not kept */ }
    form.hidden = true;
    show("");
    window.rantWeb = new WebBridge(node);
    const canvas = document.getElementById("canvas");
    await createExplorer({
        canvas,
        print: (t) => console.log(t),
        printErr: (t) => console.warn(t),
    });
    canvas.focus();
}

form.addEventListener("submit", (e) => {
    e.preventDefault();
    start({ bridge: field("bridge").value.trim(), domain: field("domain").value.trim(),
            passphrase: field("passphrase").value });
});

if (saved.passphrase && !params.has("ask")) start({ bridge: field("bridge").value, domain: field("domain").value, passphrase: saved.passphrase });
else form.hidden = false;
