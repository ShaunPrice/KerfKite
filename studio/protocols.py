"""Controller profiles: implemented transports are distinct from file postprocessors."""
from dataclasses import dataclass
from pathlib import Path
import json

@dataclass(frozen=True)
class Protocol:
    id: str
    name: str
    live_transport: bool
    family: str
    notes: str
    modes: list

PROTOCOLS = {row['id']: Protocol(**row) for row in json.loads((Path(__file__).parent/'static'/'protocols.json').read_text())}

def get_protocol(protocol):
    if not isinstance(protocol, str) or protocol not in PROTOCOLS:
        raise ValueError('Unsupported controller protocol')
    return PROTOCOLS[protocol]

def postprocess(lines, protocol='grbl', mode='router', dwell_units=''):
    profile = get_protocol(protocol)
    if mode not in profile.modes:
        raise ValueError(f'{profile.name} dynamic laser export is not implemented; choose a validated laser profile')
    if profile.family == 'grbl':
        return list(lines)
    if profile.family == 'marlin':
        return ['G4 P2000' if line == 'G4 P2' else line for line in lines if line not in ('G17','G94','G54')]
    if protocol in ('mach3', 'uccnc') and dwell_units not in ('seconds', 'milliseconds'):
        raise ValueError('Select configured dwell units: seconds or milliseconds')
    result = []
    for line in lines:
        result.append('G4 P2000' if line == 'G4 P2' and protocol in ('mach3', 'uccnc') and dwell_units == 'milliseconds' else line)
        if line == 'G90':
            result.extend(['G40', 'G49', 'G80', 'G61'])
    return result + ['M30']
