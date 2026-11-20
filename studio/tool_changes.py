"""Independent router jobs for deliberate manual tool changes; no machine access."""

import math
from .cam import generate
from .operations import router_tool_shape


def _numeric(value, name, lo, hi):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(name + ' must be numeric')
    try:
        value = float(value)
    except OverflowError:
        raise ValueError(name + ' is out of range') from None
    if not math.isfinite(value) or not lo <= value <= hi:
        raise ValueError(name + ' is out of range')
    return value


def _tool(operation):
    tool_id = _numeric(operation.get('tool_id'), 'Assign a tool number to every operation', 1, 999)
    if not tool_id.is_integer():
        raise ValueError('Tool number must be an integer in 1..999')
    name = operation.get('tool_name', f'Tool {int(tool_id)}')
    if not isinstance(name, str) or not name.strip() or len(name) > 80 or any(ord(c) < 32 for c in name):
        raise ValueError('Tool name must contain 1..80 printable characters')
    diameter = _numeric(operation.get('tool_diameter', 3.175), 'Tool diameter', 0.01, 100)
    shape = router_tool_shape(operation)
    if shape not in ('flat', 'ball', 'v', 'drill'):
        raise ValueError('Choose a flat, ball, V or drill bit')
    angle = _numeric(operation.get('v_angle', 60), 'V angle', 10, 170) if shape == 'v' else None
    return {
        'tool_id': int(tool_id),
        'tool_name': name.strip(),
        'diameter': diameter,
        'shape': shape,
        'v_angle': angle,
        **(
            {'drill_point_angle': _numeric(operation.get('drill_point_angle', 118), 'Drill point angle', 60, 170)}
            if shape == 'drill'
            else {}
        ),
    }


def plan_tool_changes(project):
    """Split contiguous same-tool operations into independently runnable stages.

    Tool identity includes ID, diameter, shape and V angle. Returning to an
    earlier tool produces another stage. Each stage is exported separately;
    never concatenate stages or assume a GRBL M6 implementation.
    """
    if not isinstance(project, dict) or project.get('mode') != 'router':
        raise ValueError('Manual tool-change plans require router mode; head changes are not supported')
    if project.get('rotary'):
        raise ValueError('Rotary tool-change plans require separate export and setup review')
    operations = project.get('operations')
    if operations is None:
        operations = [project]
    if not isinstance(operations, list) or not 1 <= len(operations) <= 100:
        raise ValueError('Use 1..100 ordered router operations')
    base = {k: v for k, v in project.items() if k not in ('operations', 'paths', 'path_depths', 'path_powers')}
    groups = []
    for index, operation in enumerate(operations):
        if not isinstance(operation, dict) or 'operations' in operation:
            raise ValueError('Nested or invalid operations are unsupported')
        for key in ('mode', 'protocol', 'width', 'height', 'dwell_units'):
            if operation.get(key, project.get(key)) != project.get(key):
                raise ValueError('A tool-change plan must use one head, protocol and work area')
        merged = {**base, **operation}
        tool = _tool(merged)
        signature = (tool['tool_id'], tool['diameter'], tool['shape'], tool['v_angle'], tool.get('drill_point_angle'))
        if not groups or groups[-1]['signature'] != signature:
            groups.append({'signature': signature, 'tool': tool, 'operations': [], 'indices': []})
        groups[-1]['operations'].append(merged)
        groups[-1]['indices'].append(index + 1)
    stages = []
    total = 0
    for index, group in enumerate(groups, 1):
        stage_project = {**base, 'operations': group['operations']}
        job = generate(stage_project)
        total += len(job['lines'])
        if total > 200000:
            raise ValueError('Tool-change plan exceeds 200000 total lines')
        ending = job['lines'][:-1] if job['lines'][-1] == 'M30' else job['lines']
        if ending[-1] != 'M5' or not ending[-2].startswith('G0 Z'):
            raise ValueError('Generated stage does not end retracted with spindle off')
        tool = group['tool']
        stages.append(
            {
                **job,
                'stage': index,
                'tool': tool,
                'operation_indices': group['indices'],
                'filename': f"stage-{index:03d}-tool-{tool['tool_id']:03d}.nc",
                'before_running': [
                    'Wait for the previous job to finish, spindle to stop and motion to cease.',
                    'Install and secure the specified tool using the machine manufacturer procedure.',
                    'Re-establish the current tool tip Z zero in G54; exported desktop profiles cancel tool-length offsets with G49. Verify retained XY work origin.',
                    'Check stock, clearances, tool geometry, feeds and this stage preview before starting it manually.',
                ],
            }
        )
    return {
        'format': 'cnc-tool-change-plan/1',
        'mode': 'router',
        'manual_only': True,
        'stages': stages,
        'tool_changes': max(0, len(stages) - 1),
        'warnings': [
            'Each stage is a separate job. Do not concatenate files or run the next stage automatically.',
            'No M6, automatic tool changer, probing, automatic resume or machine dispatch is implemented.',
            'Every tool change requires manual tool-tip work zero and a new clearance check; desktop exports cancel tool-length compensation.',
            'Tool numbers identify the requested setup, not a verified machine tool table. Confirm each stage diameter and shape physically.',
        ],
    }
