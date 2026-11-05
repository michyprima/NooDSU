#!/usr/bin/env python3
# Adds up what the shards of the test logged
import collections, sys
counts = collections.Counter(); fails = collections.Counter(); samples = {}; seen = {}; crashed = []
for path in sys.argv[1:]:
    finished = False
    for line in open(path, errors='replace'):
        word = line.rstrip('\n').split('\t')
        if not word[0]: continue
        if word[0] == 'COUNT': counts[word[1]] += int(word[2])
        elif word[0] == 'RAN': seen.setdefault(word[1], set()).add(word[2])
        elif word[0] == 'FAILS': fails[word[1]] += int(word[2])
        elif word[0] == 'FAIL':
            samples.setdefault(word[1], [])
            if len(samples[word[1]]) < 3: samples[word[1]].append(word[2][:300])
        elif word[0] == 'DONE': finished = True
    if not finished: crashed.append(path)
print('%-28s %12s' % ('Covered', 'Cases'))
for name in sorted(counts): print('%-28s %12d' % (name, counts[name]))
print('Total cases %d' % sum(v for k, v in counts.items() if k.startswith('suite:')))
for name in sorted(seen):
    print('%s: %d of 65536' % (name, len(seen[name])))
print('Mismatches: %d' % sum(fails.values()))
for name in sorted(fails):
    print('  %s: %d' % (name, fails[name]))
    for s in samples.get(name, []): print('    ' + s)
if crashed: print('Shards that didn\'t finish: ' + ' '.join(crashed))
sys.exit(1 if fails or crashed else 0)
