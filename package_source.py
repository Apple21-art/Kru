#!/usr/bin/env python3
"""Create a deterministic source-only Kru distribution, excluding build output."""
import pathlib
import stat
import sys
import zipfile

root=pathlib.Path(__file__).resolve().parent.parent
output=pathlib.Path(sys.argv[1]).resolve()
source_dirs={'src','include','lib','tests','tools','docs'}
root_files={'Makefile','.gitignore','KRU_CODEX_0_1_ALPHA_REVISED_4.txt'}
with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(root.rglob('*')):
        relative=path.relative_to(root)
        if not path.is_file() or path==output:continue
        if any(part in {'build','.git','__pycache__'} for part in relative.parts):continue
        if path.name=='core' or path.name.startswith('core.') or path.suffix=='.pyc':continue
        if len(relative.parts)==1:
            if path.name not in root_files and path.suffix!='.md':continue
        elif relative.parts[0] not in source_dirs:continue
        info=zipfile.ZipInfo(str(pathlib.PurePosixPath('Kru')/relative),date_time=(2026,1,1,0,0,0))
        info.compress_type=zipfile.ZIP_DEFLATED
        info.external_attr=(stat.S_IFREG|(path.stat().st_mode&0o777))<<16
        archive.writestr(info,path.read_bytes())
print(output)
