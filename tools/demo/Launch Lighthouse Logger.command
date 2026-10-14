#!/bin/zsh
task_project_root="${0:A:h:h:h}"
cd "$task_project_root" || exit 1
if [[ ! -x artifacts/last-lighthouse/venv/bin/python ]]; then
    print 'Create the demo Python environment first; see demos/last-lighthouse/README.md.'
    read 'reply?Press Return to close.'
    exit 1
fi
exec artifacts/last-lighthouse/venv/bin/python tools/demo/run_loopback.py
