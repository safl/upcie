#!/usr/bin/env python3
"""
Stage a nosi guest disk-image
=============================

Resolves ``[guest_image].url`` (an ``oras://`` reference) with withcache.oras,
pulls the gzip blob with curl, decompresses it, and converts it to the qcow2 at
``system-imaging.images.<image>.disk.path`` where ``guest_initialize`` finds it.
The pull is skipped when the pinned image is already staged: the resolved
content digest is recorded next to the qcow2 and re-used on a match. No-op when
``[guest_image]`` is absent. Requires ``withcache`` in the cijoe environment.

Retargetable: False (writes to the cijoe initiator's local filesystem).
"""
import logging as log
import tempfile
from argparse import ArgumentParser
from pathlib import Path


def add_args(parser: ArgumentParser):
    pass


def main(args, cijoe):
    url = cijoe.getconf("guest_image.url", None)
    if not url:
        log.info("config has no [guest_image].url; nothing to stage")
        return 0

    from withcache import oras

    image = cijoe.getconf("qemu.default_systemimage", None)
    dst = Path(cijoe.getconf(f"system-imaging.images.{image}.disk", None)["path"])
    dst.parent.mkdir(parents=True, exist_ok=True)

    # Skip the multi-GB pull when the pinned image is already on disk. The
    # content digest is recorded in a stamp next to the qcow2; a digest bump in
    # the config no longer matches and triggers a fresh stage.
    digest = oras.parse_ref(url).digest
    stamp = Path(f"{dst}.digest")
    if digest and dst.exists() and stamp.exists() and stamp.read_text().strip() == digest:
        log.info(f"image already staged at {dst} ({digest}); skipping pull")
        return 0

    log.info(f"staging {url} -> {dst}")
    resolved = oras.resolve_ref(url)
    bearer = resolved.headers["Authorization"]

    # Workdir on dst's filesystem; the container /tmp overlay is too small.
    with tempfile.TemporaryDirectory(dir=dst.parent) as workdir:
        gz = Path(workdir) / "guest.img.gz"
        raw = Path(workdir) / "guest.img"
        for cmd in [
            f"curl -fL --retry 5 --retry-all-errors -C - -H 'Authorization: {bearer}' "
            f"-o {gz} '{resolved.blob_url}'",
            f"gunzip {gz}",
            f"qemu-img convert -f raw {raw} -O qcow2 {dst}",
            f"qemu-img resize {dst} +8G",
        ]:
            err, _ = cijoe.run_local(cmd)
            if err:
                return err

    stamp.write_text(digest or "")
    return 0
