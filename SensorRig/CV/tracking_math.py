"""Small deterministic motion/assignment primitives; no hardware or ML imports."""
from functools import lru_cache
import math


def assignment(costs, unmatched=12.0):
    """Minimum total one-to-one cost, with explicit unmatched rows/columns.

    Bounded bitmask DP is suitable for the LD2450's three returns and eight
    rendered people. Disallowed edges are infinity. Tie-breaking is stable.
    """
    if not costs or not costs[0]:
        return {}
    rows, cols = len(costs), len(costs[0])
    if cols > 16 or rows > 32:
        raise ValueError('Assignment input exceeds bounded tracker budget')

    @lru_cache(None)
    def solve(row, mask):
        if row == rows:
            return unmatched * (cols - mask.bit_count()), ()
        tail, pairs = solve(row + 1, mask)
        best = (unmatched + tail, pairs)
        for col, cost in enumerate(costs[row]):
            if mask & (1 << col) or not math.isfinite(cost):
                continue
            tail, pairs = solve(row + 1, mask | (1 << col))
            candidate = (cost + tail, ((row, col),) + pairs)
            if candidate[0] < best[0]:
                best = candidate
        return best
    return dict(solve(0, 0)[1])


class MotionFilter:
    """Independent constant-velocity Kalman axes; coordinates in metres/seconds.

    No prediction mutates freshness. Equal/older capture times never update.
    Process/measurement variances are tuning parameters, not calibrated claims.
    """
    def __init__(self, right, forward, at, sigma=.15):
        self.axes = [[right, 0., sigma*sigma, 0., 1.],
                     [forward, 0., sigma*sigma, 0., 1.]]
        self.at = at

    def predicted(self, at):
        dt = max(0., min(1., at-self.at))
        q = 4.0  # acceleration process variance
        return [[p+v*dt, v, pp+2*dt*pv+dt*dt*vv+q*dt**4/4,
                 pv+dt*vv+q*dt**3/2, vv+q*dt*dt]
                for p,v,pp,pv,vv in self.axes]

    def innovation(self, right, forward, at, sigma):
        axes = self.predicted(at)
        return sum((z-a[0])**2/(a[2]+sigma*sigma)
                   for z,a in zip((right,forward),axes))

    def update(self, right, forward, at, sigma=.15, gate=16.):
        if at <= self.at:
            return False
        if self.innovation(right,forward,at,sigma) > gate:
            return False
        axes = self.predicted(at)
        for a,z in zip(axes,(right,forward)):
            p,v,pp,pv,vv = a
            s = pp+sigma*sigma
            a[:] = [p+pp/s*(z-p), v+pv/s*(z-p),
                    max(1.e-8, pp-pp*pp/s), pv-pp*pv/s,
                    max(1.e-8, vv-pv*pv/s)]
        self.axes, self.at = axes, at
        return True

    def state(self):
        return dict(right_m=self.axes[0][0], forward_m=self.axes[1][0],
                    velocity_right_mps=self.axes[0][1], velocity_forward_mps=self.axes[1][1],
                    variance_right_m2=self.axes[0][2], variance_forward_m2=self.axes[1][2])
