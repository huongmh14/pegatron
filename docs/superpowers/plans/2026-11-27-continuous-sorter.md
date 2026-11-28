# Continuous measurement and L/S servo sorting

Goal: measure each complete ToF passage, discard the first/last 20% of samples in acquisition order, and classify the median of the middle 60%. Station 1 pushes L; station 2 pushes S; M passes.

Architecture: bounded C++ buffers in `sorter_core.h`; one pending FIFO ticket per detected object in `station1_servo_trial_logic.h`; integration in the production and station-1 bench firmware. No dependencies or hardware actuation during verification.

Constraints: reserve at the first occupied sample, complete only the matching ticket, consume early/busy arrivals once, preserve invalid/pass placeholders, and inhibit automatic pushes after loss of queue synchronization. Thresholds and servo timings still require hardware calibration.

- [x] Replace three-sample/peak detection with complete-passage trimming, minimum sample count, overflow and timeout handling.
- [x] Change routing to L/S/M and reject heights outside configured ranges.
- [x] Add FIFO reservation/completion and account for LM393 arrivals during servo motion.
- [x] Integrate firmware, update focused compile-time checks, build production and bench targets.
- [x] Record verified results, operating limits and restart procedure in project documentation.

Checks: native C++17 static assertions for temporal trimming, invalid samples, boundaries, time wraparound, pending tickets, busy arrivals, synchronization faults and routing; Pico CMake builds for `BangTaiPhanLoai` and `TestServoTram1`.

Verification: native and ARM compile-time checks pass. Both firmware targets compile/link to ELF; the overall build remains blocked at UF2 post-processing by Device Guard (picotool exit 4551). No flash or hardware actuation performed.
