"""Download the pinned, CPU-only OCR runtime and public models into build/."""
import hashlib
import shutil
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / "build" / "ocr-deps"
OUTPUT = ROOT / "build" / "ocr"
ASSETS = {
    "det.onnx": (
        "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_mobile.onnx",
        "4d97c44a20d30a81aad087d6a396b08f786c4635742afc391f6621f5c6ae78ae"),
    "rec.onnx": (
        "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile.onnx",
        "5825fc7ebf84ae7a412be049820b4d86d77620f204a041697b0494669b1742c5"),
    "ort.zip": (
        "https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip",
        "78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581"),
    "PaddleOCR-LICENSE.txt": (
        "https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/v3.0.0/LICENSE",
        "3840c5c0c61c294264d2dd77b8777be6ddd90121ef4e0e64abcd22edea581d6e"),
}


def main():
    CACHE.mkdir(parents=True, exist_ok=True)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, (url, expected) in ASSETS.items():
        path = CACHE / name
        if not path.exists():
            partial = path.with_suffix(path.suffix + ".part")
            with urllib.request.urlopen(url, timeout=120) as source, partial.open("wb") as target:
                shutil.copyfileobj(source, target)
            partial.replace(path)
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise RuntimeError(f"Hash mismatch: {path}; remove this cached file and retry")
        if name != "ort.zip":
            shutil.copy2(path, OUTPUT / name)
        print(f"Verified {name}", flush=True)
    prefix = "onnxruntime-win-x64-1.20.1/"
    with zipfile.ZipFile(CACHE / "ort.zip") as archive:
        for name in archive.namelist():
            relative = name.removeprefix(prefix)
            if relative.startswith("include/") and not name.endswith("/"):
                target = CACHE / "include" / Path(relative).name
            elif relative in ("lib/onnxruntime.dll", "lib/onnxruntime_providers_shared.dll"):
                target = OUTPUT / Path(relative).name
            elif relative in ("LICENSE", "ThirdPartyNotices.txt"):
                target = OUTPUT / ("ONNXRuntime-" + ("LICENSE.txt" if relative == "LICENSE" else relative))
            else:
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(name))
    (OUTPUT / "SOURCES.txt").write_text(
        "PP-OCRv5 mobile models: PaddleOCR (Baidu), Apache-2.0. ONNX conversions: RapidAI/RapidOCR.\n"
        "ONNX Runtime 1.20.1: Microsoft, MIT. No PixPin code or models are included.\n"
        + "\n".join(f"{name}\n{url}\nSHA256 {digest}\n" for name, (url, digest) in ASSETS.items()),
        encoding="utf-8")
    print(f"OCR assets ready: {OUTPUT}")


if __name__ == "__main__":
    main()
