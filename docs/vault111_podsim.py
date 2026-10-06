# Mirrors CoSyncVault111::RemapProxyIntoPod / Reapply rules (index-level).
POD_COUNT = 7
def display(k, j, local_reapplied, remote_reapplied, n):
    """Where machine k shows player j (index). 'S'=scripted pod (real pos), int = pod index, ('stand',i) after reapply."""
    if j == k:  # local player: vanilla -> scripted pod until its own reapply
        return ('stand', k) if (local_reapplied and k > 0) else 'S'
    near_scripted = not remote_reapplied[j]  # before its reapply a player is at the scripted pod on its machine
    if j == 0:
        # host never moves on reapply: it really is at the scripted pod
        if k <= 0 or local_reapplied: return 'S'
        return k
    if remote_reapplied[j]: return ('stand', j)
    return j
ok = True
for n in range(1, 9):
    # freeze: nobody reapplied
    for k in range(n):
        occ = {}
        for j in range(n):
            d = display(k, j, False, [False]*n, n)
            key = d if d != 'S' else 0
            if isinstance(key, int) and key >= POD_COUNT: key = ('aisle', key)
            occ.setdefault(key, []).append(j)
        dup = {p: v for p, v in occ.items() if len(v) > 1 and not (isinstance(p, tuple) and p[0]=='aisle')}
        if dup: ok = False; print("freeze dup n=%d k=%d %s" % (n, k, dup))
    # everybody reapplied
    for k in range(n):
        shown = [display(k, j, True, [True]*n, n) for j in range(n)]
        real = ['S' if j == 0 else ('stand', j) for j in range(n)]
        if shown != real: ok = False; print("after mismatch n=%d k=%d %s vs %s" % (n, k, shown, real))
# logical owner of every slot pod (index>=1) while frozen, per machine
n = 4
for k in range(n):
    owners = {}
    for j in range(n):
        d = display(k, j, False, [False]*n, n)
        owners[0 if d == 'S' else d] = j
    print("machine %d sees pods:" % k, dict(sorted(owners.items())))
print("ALL CHECKS PASSED" if ok else "FAILURES")
