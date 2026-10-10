"""Assemble the minimal runtime release with offline OCR and VC redistributable."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import sys
import zipfile
from datetime import datetime

ROOT = Path(__file__).resolve().parents[1]
MODULES = ('qtbase', 'qtsvg', 'qtimageformats')
RUNTIME = {
    'Qt5Core.dll': 'qtbase', 'Qt5Gui.dll': 'qtbase', 'Qt5Widgets.dll': 'qtbase',
    'Qt5Svg.dll': 'qtsvg', 'libEGL.dll': 'qtbase', 'libGLESV2.dll': 'qtbase',
    'platforms/qwindows.dll': 'qtbase', 'styles/qwindowsvistastyle.dll': 'qtbase',
    'iconengines/qsvgicon.dll': 'qtsvg', 'imageformats/qsvg.dll': 'qtsvg',
    'imageformats/qgif.dll': 'qtbase', 'imageformats/qico.dll': 'qtbase',
    'imageformats/qjpeg.dll': 'qtbase',
    **{'imageformats/q' + name + '.dll': 'qtimageformats'
       for name in ('icns', 'tga', 'tiff', 'wbmp', 'webp')},
}


def sha(path):
    with path.open('rb') as stream:
        result = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
        return result.hexdigest()


def relocated_core(data):
    """Reproduce windeployqt's installation-prefix relocation, preserving binary size."""
    marker = b'qt_prfxpath='
    if data.count(marker) != 1:
        raise RuntimeError('Unexpected Qt prefix marker count')
    start = data.index(marker) + len(marker)
    end = data.index(b'\0', start)
    if end <= start:
        raise RuntimeError('Empty Qt installation prefix')
    return data[:start] + b'.' + b'\0' * (end - start - 1) + data[end:]


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8'))


def files_under(root):
    return sorted(p for p in root.rglob('*') if p.is_file())


def zip_files(path, entries):
    with zipfile.ZipFile(path, 'x', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for source, name in entries:
            archive.write(source, name)
    # Archive integrity is packaging validation, not application/desktop testing.
    with zipfile.ZipFile(path) as archive:
        bad = archive.testzip()
        if bad:
            raise RuntimeError('Archive CRC failure: ' + bad)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-root', type=Path, default=Path(r'D:\Qt\Qt5.12.9\5.12.9\msvc2017_64'))
    parser.add_argument('--qt-source', type=Path, default=Path(r'D:\Qt\Qt5.12.9\5.12.9\Src'))
    args = parser.parse_args()
    qt, source = args.qt_root.resolve(), args.qt_source.resolve()
    if 'QT_VERSION = 5.12.9' not in (qt / 'mkspecs/qconfig.pri').read_text():
        raise RuntimeError('Only audited Qt 5.12.9 is supported; audit new versions first.')
    for module in MODULES:
        if 'MODULE_VERSION = 5.12.9' not in (source / module / '.qmake.conf').read_text():
            raise RuntimeError('Wrong source version: ' + module)
    binary = ROOT / 'build/LongShot.exe'
    if not binary.is_file():
        raise RuntimeError('Compile LongShot before packaging.')
    # Ensure source/build inputs have not changed since the compiled executable.
    inputs = [p for d in ('src', 'resources') for p in files_under(ROOT / d)] + [ROOT / 'CMakeLists.txt']
    if any(p.stat().st_mtime_ns > binary.stat().st_mtime_ns for p in inputs):
        raise RuntimeError('Source newer than binary; rebuild before packaging.')
    known = set(RUNTIME)
    deployed = {p.relative_to(ROOT / 'dist').as_posix() for p in (ROOT / 'dist').glob('*.dll')}
    for directory in ('platforms', 'styles', 'imageformats', 'iconengines'):
        deployed.update(p.relative_to(ROOT / 'dist').as_posix()
                        for p in (ROOT / 'dist' / directory).glob('*.dll'))
    if deployed != known:
        raise RuntimeError('Runtime inventory changed: ' + repr(deployed ^ known))
    records = []
    for name, module in RUNTIME.items():
        installed = qt / ('plugins' if '/' in name else 'bin') / name
        deployed_file = ROOT / 'dist' / name
        patch = None
        if sha(installed) != sha(deployed_file):
            if name != 'Qt5Core.dll' or relocated_core(installed.read_bytes()) != deployed_file.read_bytes():
                raise RuntimeError('Deployed DLL differs from audited Qt install: ' + name)
            patch = 'qt_prfxpath changed to . with NUL padding (windeployqt relocation)'
        records.append({'file': name, 'module': module, 'version': '5.12.9',
                        'installed_sha256': sha(installed), 'sha256': sha(deployed_file), 'patch': patch})
    redist = ROOT / 'dist/vc_redist.x64.exe'
    # Microsoft-signed VS 18 redistributable 14.51.36247.0; re-audit before changing this pin.
    if sha(redist) != '843068991daaa1f73ad9f6239bce4d0f6a07a51f18c37ea2a867e9beca71295c':
        raise RuntimeError('VC redistributable differs from the verified Microsoft installer.')
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    version = re.search(r'project\(LongShot VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text()).group(1)
    release = ROOT / 'dist/releases' / ('LongShot-' + version + '-minimal-' + stamp)
    release.mkdir(parents=True, exist_ok=False)
    ocr = ROOT / 'build/ocr'
    required_ocr = ('det.onnx', 'rec.onnx', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll',
                    'PaddleOCR-LICENSE.txt', 'ONNXRuntime-LICENSE.txt',
                    'ONNXRuntime-ThirdPartyNotices.txt', 'SOURCES.txt')
    for name in required_ocr:
        if not (ocr / name).is_file():
            raise RuntimeError('Missing OCR asset; run python tools/fetch_ocr.py: ' + name)
    (release / 'ocr').mkdir()
    for name in required_ocr:
        shutil.copyfile(ocr / name, release / 'ocr' / name)
    shutil.copyfile(redist, release / 'vc_redist.x64.exe')
    print('Assembling ' + str(release), flush=True)
    licenses = release / 'licenses'
    write(licenses / 'Microsoft-VC-Redistributable-NOTICE.txt',
          'Microsoft Visual C++ Redistributable x64 14.51.36247.0\n'
          'Copyright Microsoft Corporation. Distributed unchanged from the installed Visual Studio redistributables.\n'
          'This installer is proprietary Microsoft software, not GPL-licensed LongShot code.\n'
          'Installation is subject to the Microsoft license terms displayed by the installer.\n'
          'Run vc_redist.x64.exe only if the required runtime is missing or outdated.\n'
          'https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist\n')
    rows = []
    for module in MODULES:
        for license_path in sorted((source / module).glob('LICENSE*')):
            if license_path.is_file() and 'QT-LICENSE-AGREEMENT' not in license_path.name:
                target = licenses / 'qt-5.12.9' / module / license_path.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(license_path, target)
        for metadata in sorted((source / module).rglob('qt_attribution.json')):
            data = json.loads(metadata.read_text(encoding='utf-8'), strict=False)
            for item in data if isinstance(data, list) else [data]:
                relative = metadata.relative_to(source).as_posix()
                row = {'module': module, 'metadata': relative, **item}
                license_name = item.get('LicenseFile')
                if license_name:
                    license_path = (metadata.parent / license_name).resolve()
                    license_path.relative_to(source)
                    target = licenses / 'qt-5.12.9' / license_path.relative_to(source)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(license_path, target)
                    row['BundledLicense'] = target.relative_to(release).as_posix()
                rows.append(row)
    # Files referenced by license summaries, plus patent and attribution notices.
    extra = ('qtbase/src/3rdparty/libjpeg/src/README.ijg',
             'qtbase/src/3rdparty/freetype/docs/FTL.TXT',
             'qtbase/src/3rdparty/freetype/docs/GPLv2.TXT',
             'qtimageformats/src/3rdparty/libwebp/PATENTS',
             'qtimageformats/src/3rdparty/libwebp/AUTHORS')
    for name in extra:
        target = licenses / 'qt-5.12.9' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, target)
    write(licenses / 'qt-attributions.json', json.dumps(rows, ensure_ascii=False, indent=2) + '\n')
    minimal = {'Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'Qt5Svg.dll', 'platforms/qwindows.dll',
               'styles/qwindowsvistastyle.dll', 'imageformats/qico.dll', 'imageformats/qjpeg.dll'}
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
        shutil.copyfile(ROOT / name, release / name)
    shutil.copyfile(binary, release / 'LongShot.exe')
    for name in sorted(minimal):
        target = release / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / 'dist' / name, target)
    write(release / 'README.txt',
          'LongShot 最小运行包\n\n解压后运行 LongShot.exe。保存图片后退出旧托盘进程再启动新版。\n'
          '在单个显示器内框选，选区旁直接画矩形、箭头、文字和马赛克；完成默认复制。\n长截图、查看上一张与 PNG 导出；导入支持 PNG/JPEG/ICO/BMP 等 Qt 内置或附带格式。\n'
          '离线文字识别默认使用 PP-OCRv5 中英混合模型，请保留 ocr 目录。\n'
          '本包不含源码或测试。\n'
          '缺少或需要更新 MSVC 运行库时，运行随包 vc_redist.x64.exe（14.51.36247.0）。微软官方下载：\n'
          'https://aka.ms/vc14/vc_redist.x64.exe\n\n'
          '许可：GPL-3.0-only；第三方许可见 THIRD_PARTY_NOTICES.md 与 licenses。\n'
          '第三方声明是模块级完整清单，不表示每个条目都编译进此最小运行包。\n'
          '项目源码：https://github.com/NiceBlueChai/LongShot\n')
    manifest = {'created': stamp, 'version': version, 'mode': 'minimal', 'license': 'GPL-3.0-only',
                'runtime': [record for record in records if record['file'] in minimal],
                'validation': 'Hashes and ZIP integrity only; no application tests run.',
                'files': {p.relative_to(release).as_posix(): sha(p) for p in files_under(release)}}
    write(release / 'MANIFEST.json', json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    final = release.parent / (release.name + '.zip')
    zip_files(final, [(p, release.name + '/' + p.relative_to(release).as_posix())
                     for p in files_under(release)])
    write(final.with_suffix('.sha256'), sha(final) + '  ' + final.name + '\n')
    print('COMPLETE: ' + str(final), flush=True)
    print('MiB: ' + str(round(final.stat().st_size / 1048576, 2)), flush=True)


if __name__ == '__main__':
    main()
