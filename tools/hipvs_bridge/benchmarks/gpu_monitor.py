"""Select only the GPU visible to this single-GPU benchmark instance."""
from functools import lru_cache
import json
from pathlib import Path
import re
import subprocess

@lru_cache(maxsize=1)
def gpu_memory_source():
    devices=json.loads(subprocess.check_output(['rocm-smi','--showbus','--json'],text=True))
    buses=[value['PCI Bus'] for value in devices.values() if isinstance(value,dict) and 'PCI Bus' in value]
    if len(buses)!=1:
        raise RuntimeError('Expected exactly one visible GPU; cannot safely select its VRAM counter')
    bus=buses[0].lower()
    if not re.fullmatch(r'[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]',bus):
        raise RuntimeError('Invalid visible GPU PCI address')
    path=Path('/sys/bus/pci/devices')/bus/'mem_info_vram_used'
    initial=int(path.read_text())
    if initial<0:raise RuntimeError('Invalid GPU VRAM counter')
    return path,{'gpu_pci_bus':bus,'gpu_vram_counter':str(path),'gpu_initial_vram_bytes':initial}
