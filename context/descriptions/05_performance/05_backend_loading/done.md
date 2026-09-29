# Backend loading

Gurobi spans cover environment creation, variable arrays, model creation, individual linear-row submission, quadratic/general/indicator rows and quadratic objectives. No loading strategy is changed.

HiGHS spans cover variable types, row/range coordinate arrays, coefficient scaling, CSR packing, `passModel` and Hessian loading. Existing temporary arrays and copies remain. Boundary RSS and total process peak are recorded independently from optimization.

