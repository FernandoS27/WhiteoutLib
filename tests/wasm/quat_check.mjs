import { Whiteout } from "../../package/index.js";
const wo = await Whiteout();
const M = wo.module;
const tr = new M.MdxTrackQuaternion();
try {
    console.log("created Track<Quaternion>");
    const keys = tr.keys;     // <- expected to throw if vector<Quaternion> unregistered
    console.log("keys ok, size=", keys.size());
    keys.delete();
} catch (e) {
    console.log("FAIL accessing keys:", e?.message ?? e);
}
tr.delete();
