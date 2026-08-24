# Kernel Time Subsystem

## TODOs

### Unified Clock Calibration

### Conversion between Time and Ticks

#### From ticks to time

First, suppose the following relationship holds:

$$ time = ticks \times \frac{1}{freq} $$

Since $1 / freq$ is generally not an integer, we approximate it as a binary fixed-point rational:

$$ \frac{1}{freq} \approx \frac{m}{2^s} $$

Substituting:

$$ time = (ticks \times m) \gg s $$

#### From time to ticks

Inverting the relationship:

$$ ticks = \left\lceil time \times freq \right\rceil = \left\lceil \frac{time \times 2^s}{m} \right\rceil $$

---

### Precision Analysis

#### Error bound

The multiplier $m$ is chosen by rounding to nearest:

$$ m = \left\lfloor \frac{2^s}{freq} + \frac{1}{2} \right\rfloor $$

The approximation error for a single conversion is bounded by:

$$ \left| \frac{m}{2^s} - \frac{1}{freq} \right| \leq \frac{1}{2^{s+1}} $$

After multiplying by $ticks$, the absolute error in the output is at most:

$$ \Delta time \leq \frac{ticks}{2^{s+1}} $$

**Key insight**: larger $s$ reduces error, but $m$ grows proportionally ($m \approx 2^s / freq$),
which may cause overflow in the intermediate product $ticks \times m$.

#### Choosing the shift $s$

The algorithm in `BinaryFixedPointRatio::from_ratio` selects the largest feasible $s$ subject to three constraints:

$$
\begin{aligned}
\text{(1)}\quad & m \leq max\_multiplier \\
\text{(2)}\quad & scale(max\_input) \leq max\_output \quad \text{即} \quad \frac{max\_input \times m}{2^s} \leq max\_output \\
\text{(3)}\quad & s \leq max\_shift
\end{aligned}
$$

It searches from $s = max\_shift$ downward, taking the first candidate that satisfies all constraints.
Since larger $s$ gives better precision, the first feasible candidate is optimal.

#### Rounding modes

The conversion supports two rounding variants:

| Mode | Expression | Use case |
|------|------------|----------|
| Floor (truncate) | $(value \times m) \gg s$ | ticks → time (reporting elapsed time; conservative for deadlines) |
| Ceiling | $(value \times m + 2^s - 1) \gg s$ | time → ticks (ensures the tick count is sufficient to cover the requested duration) |

Using floor for ticks→time means the reported time never exceeds the actual elapsed time.
Using ceiling for time→ticks guarantees a sleep/delay lasts *at least* the requested duration.