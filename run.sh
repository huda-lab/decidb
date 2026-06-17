#!/usr/bin/env bash
clear
set -o pipefail

cd build/release



./decidb ../../decidb.db <<'EOF'
.mode box

PRAGMA diagnose_decide='auto';

SELECT p_partkey, make, buy, promo
FROM part
WHERE p_size <= 5
DECIDE make IS REAL, buy IS REAL, promo IS REAL
SUCH THAT make >= 0 AND buy >= 0 AND promo >= 0
     AND make <= 500
     AND make + buy >= 50
     AND promo >= make
     AND SUM(buy * p_retailprice) <= 100000
MAXIMIZE SUM(make * (p_retailprice * 0.30) + buy * (p_retailprice * 0.10) + promo * 2);

SELECT * FROM decide_diagnostics();
EOF

