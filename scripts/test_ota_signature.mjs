import { generateKeyPairSync, sign } from "node:crypto";
import { spawnSync } from "node:child_process";

const executable = process.argv[2];
if (!executable) throw new Error("usage: node scripts/test_ota_signature.mjs <host-test-binary>");

const { publicKey, privateKey } = generateKeyPairSync("ed25519");
const publicKeyHex = publicKey.export({ format: "der", type: "spki" }).subarray(-32).toString("hex");
const firmwareTarget = "b1-v4-c";
const version = "1.2.3-rc.1";
const size = "123456";
const sha256 = Buffer.from(Array.from({ length: 32 }, (_, i) => i)).toString("hex");
const message = Buffer.from(`Powerlytics OTA v1\n${firmwareTarget}\n${version}\n${size}\n${sha256}\n`, "utf8");
const signatureHex = sign(null, message, privateKey).toString("hex");

function run(target, firmwareVersion = version, firmwareSize = size, checksum = sha256, keyHex = publicKeyHex) {
  return spawnSync(executable, [target, firmwareVersion, firmwareSize, checksum, keyHex, signatureHex], { encoding: "utf8" });
}

const valid = run(firmwareTarget);
if (valid.status !== 0) throw new Error(`firmware C verifier rejected the valid cross-language signature: ${valid.stderr}`);

const wrongTarget = run("b1-beta-c");
if (wrongTarget.status !== 1) throw new Error("firmware C verifier accepted a signature for a different board target");

const wrongVersion = run(firmwareTarget, "1.2.4");
if (wrongVersion.status !== 1) throw new Error("firmware C verifier accepted a signature for a different version");

const wrongSize = run(firmwareTarget, version, "123457");
if (wrongSize.status !== 1) throw new Error("firmware C verifier accepted a signature for a different image size");

const wrongHash = run(firmwareTarget, version, size, "ff".repeat(32));
if (wrongHash.status !== 1) throw new Error("firmware C verifier accepted a signature for a different image hash");

const malformedHash = run(firmwareTarget, version, size, "zz".repeat(32));
if (malformedHash.status !== 65) throw new Error("firmware C verifier accepted malformed hash bytes");

const malformed = run(firmwareTarget, "bad\nversion");
if (malformed.status !== 65) throw new Error("firmware C verifier accepted a newline in the signed version field");

console.log("OTA Ed25519 C/Node signature contract passed (valid, wrong target/version/size/hash, malformed fields)");
