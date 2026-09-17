# Test corpus

This directory is reserved for deterministic SInput HIL tests.

Recommended layout:

    tests/
      packets/
        state-standard.bin
        state-imu.bin
        state-battery.bin
      expected/
        evtest-standard.txt
      scripts/
        replay-...

Do not commit captures containing private device identifiers unless they are
explicitly intended as public test fixtures.
