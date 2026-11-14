#!/usr/bin/env python3
"""Check the two documented integration boundaries fixed in review L-04."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
stage = (root / 'docs/architecture/ssc-async-stage1.md').read_text(encoding='utf-8')
issues = (root / 'docs/open-issues.md').read_text(encoding='utf-8')
assert 'Historical stage 1' in stage and '../open-issues.md' in stage
assert 'not yet call' not in stage and 'next\nimplementation step' not in stage
assert 'strncmp(foe_cfg->name, filename, strlen(foe_cfg->name))' not in issues
assert 'strncmp (foe_file_name, foe_cfg->files[i].name, num_chars)' in issues
print('PASS: asynchronous status and SOES filename documentation')
