"""Benchmark installed extension against explicit Python on identical CAM jobs."""
import json,platform,statistics,sys
from pathlib import Path
from studio import relief
from .stress_cam import workload

assert relief._relief_native is not None, 'This validation requires the compiled extension'
result={'platform':platform.platform(),'python_version':sys.version,'extension':relief._relief_native.__file__}
for backend in ('python','native'):
    result[backend]=[workload(backend=backend) for _ in range(3)]
assert len({r['sha256'] for backend in ('python','native') for r in result[backend]})==1
result['hashes_match']=True
result['medians']={b:{'pipeline':statistics.median(sum(r['stages'].values()) for r in result[b]),
    'relief':statistics.median(r['stages']['relief'] for r in result[b])} for b in ('python','native')}
Path(sys.argv[1] if len(sys.argv)>1 else 'native-results.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
