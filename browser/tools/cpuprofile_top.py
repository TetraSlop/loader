# Summarise a V8 .cpuprofile (node --cpu-prof) by self time per function, for profiling the Wasm build. Build with
# --profiling-funcs so Wasm functions keep their names.
#   python browser/tools/cpuprofile_top.py <file.cpuprofile> [--top 25]
import argparse
import collections
import json


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('profile')
    parser.add_argument('--top', type=int, default=25)
    args = parser.parse_args()
    profile = json.load(open(args.profile))
    nodes = {n['id']: n for n in profile['nodes']}
    counts = collections.Counter(profile['samples'])
    self_time = collections.Counter()
    for node_id, hits in counts.items():
        frame = nodes[node_id]['callFrame']
        self_time[frame['functionName'] or '(anonymous) ' + frame['url'].rsplit('/', 1)[-1]] += hits
    total = sum(self_time.values())
    for name, hits in self_time.most_common(args.top):
        print('%6.2f%%  %s' % (100 * hits / total, name[:150]))


if __name__ == '__main__':
    main()
