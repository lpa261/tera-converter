#!/usr/bin/env python3
"""Install a locally supplied model after checking its published SHA-256."""
import argparse
import fcntl
import stat
import hashlib
import os
from pathlib import Path
import shutil
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('model', type=Path)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--destination', type=Path,
                        default=Path(os.environ.get('XDG_DATA_HOME',Path.home()/'.local/share'))/'teraconvert/models')
    parser.add_argument('--replace', action='store_true')
    args = parser.parse_args()
    expected=args.sha256.lower()
    if len(expected)!=64 or any(c not in '0123456789abcdef' for c in expected):
        parser.error('Expected a complete SHA-256 digest from the selected model manifest')
    descriptor=os.open(args.model,os.O_RDONLY|os.O_NONBLOCK|os.O_CLOEXEC)
    with os.fdopen(descriptor,'rb') as source:
        st=os.fstat(source.fileno())
        if not stat.S_ISREG(st.st_mode) or not 1_000_000 <= st.st_size <= 1024*1024*1024:
            parser.error('Model must be a bounded regular ONNX file (1 MiB–1 GiB)')
        args.destination.mkdir(parents=True,exist_ok=True)
        lock_fd=os.open(args.destination/'.model.lock',os.O_RDWR|os.O_CREAT|os.O_CLOEXEC|os.O_NOFOLLOW,0o600)
        fcntl.flock(lock_fd,fcntl.LOCK_EX)
        target=args.destination/'rembg.onnx'
        receipt=args.destination/'rembg.onnx.sha256'
        if (os.path.lexists(target) or os.path.lexists(receipt)) and not args.replace:
            parser.error('A model/receipt already exists; use --replace explicitly')
        stage=Path(tempfile.mkdtemp(prefix='.model-',dir=args.destination))
        try:
            candidate=stage/'rembg.onnx'
            with candidate.open('xb') as output:
                checksum=hashlib.sha256();count=0
                while chunk:=source.read(1024*1024):
                    count+=len(chunk)
                    if count>st.st_size:raise ValueError("Source model grew during verification")
                    checksum.update(chunk);output.write(chunk)
                output.flush();os.fsync(output.fileno())
            if count!=st.st_size or os.fstat(source.fileno()).st_mtime_ns!=st.st_mtime_ns:
                raise ValueError("Source model changed during verification")
            if checksum.hexdigest()!=expected:
                raise ValueError('Model checksum does not match; existing model preserved')
            (stage/'rembg.onnx.sha256').write_text(expected+'\n',encoding='ascii')
            if args.replace:
                os.replace(candidate,target)
                os.replace(stage/'rembg.onnx.sha256',receipt)
            else:
                os.link(candidate,target)
                try:
                    os.link(stage/'rembg.onnx.sha256',receipt)
                except Exception:
                    target.unlink()
                    raise
            print(f'Installed verified RMBG ONNX model: {target}')
        finally:
            shutil.rmtree(stage)
            os.close(lock_fd)


if __name__=='__main__':
    main()
