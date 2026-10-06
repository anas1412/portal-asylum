"""Send commands to the running mod and wait for each to run. Usage: drive.py "cmd" "wait 1.5" "shot name" ...
A 'shot NAME' also waits for the file and converts it to run/NAME.png (640 wide)."""
import sys, time, os, subprocess
RUN = os.path.expanduser('~/outlast-portal-gun/run')
def log(): return open(f'{RUN}/mod.log', errors='replace').read()
for c in sys.argv[1:]:
    if c.startswith('wait '): time.sleep(float(c.split()[1])); continue
    if c.startswith('until '):  # until <text in log>
        t = c[6:]; d = time.time() + 120
        while t not in log() and time.time() < d: time.sleep(0.5)
        continue
    n = log().count(f'> {c}\n')
    open(f'{RUN}/cmd', 'a').write(c + '\n')
    d = time.time() + 30
    while log().count(f'> {c}\n') <= n and time.time() < d: time.sleep(0.2)
    if c.startswith('shot '):
        name = c.split()[1]; d = time.time() + 10
        while f'{name}.ppm' not in log().split(f'> {c}\n')[-1] and time.time() < d: time.sleep(0.2)
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', f'{RUN}/{name}.ppm', '-vf', 'scale=640:-1', f'{RUN}/{name}.png'])
    time.sleep(0.3)
