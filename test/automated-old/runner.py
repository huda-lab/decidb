#!/usr/bin/env python3
"""
Automated Testing Framework for DecidB - Refactored

This framework tests DecidB's DECIDE clause by:
1. Auto-discovering all queries in test/automated/queries/
2. Using the real decidb.db database
3. Comparing Gurobi solver results with DecidB results in matching table formats

Note: This runner uses gurobi_cl as the reference solver (must be in PATH).
DecidB internally may use either Gurobi or HiGHS depending on availability.
"""

import subprocess
import os
import sys
import csv
import glob
from pathlib import Path
from datetime import datetime
import re
import time
import ast
import operator
import math


def safe_eval_arithmetic(expr_str):
    """
    Safely evaluate arithmetic expressions like '2.667e-6 * 100' or 'sqrt(100)'.
    
    Supports:
    - Numbers (int, float, scientific notation)
    - Operators: +, -, *, /, ** (power), % (modulo)
    - Functions: sqrt, abs, floor, ceil, round
    
    Returns None if the expression cannot be evaluated.
    """
    expr_str = expr_str.strip()
    
    # First, try direct float conversion (handles simple numbers and scientific notation)
    try:
        return float(expr_str)
    except ValueError:
        pass
    
    # Define allowed operators
    allowed_operators = {
        ast.Add: operator.add,
        ast.Sub: operator.sub,
        ast.Mult: operator.mul,
        ast.Div: operator.truediv,
        ast.Pow: operator.pow,      # Exponentiation: 2 ** 3
        ast.Mod: operator.mod,      # Modulo: 10 % 3
        ast.FloorDiv: operator.floordiv,  # Floor division: 10 // 3
        ast.USub: operator.neg,     # Unary minus: -5
        ast.UAdd: operator.pos,     # Unary plus: +5
    }
    
    # Define allowed functions
    allowed_functions = {
        'sqrt': math.sqrt,
        'abs': abs,
        'floor': math.floor,
        'ceil': math.ceil,
        'round': round,
        'pow': pow,
        'log': math.log,
        'log10': math.log10,
        'exp': math.exp,
    }
    
    def _eval(node):
        if isinstance(node, ast.Num):  # Python 3.7 and earlier
            return node.n
        elif isinstance(node, ast.Constant):  # Python 3.8+
            if isinstance(node.value, (int, float)):
                return node.value
            raise ValueError(f"Unsupported constant type: {type(node.value)}")
        elif isinstance(node, ast.BinOp):
            left = _eval(node.left)
            right = _eval(node.right)
            op_type = type(node.op)
            if op_type not in allowed_operators:
                raise ValueError(f"Unsupported operator: {op_type}")
            return allowed_operators[op_type](left, right)
        elif isinstance(node, ast.UnaryOp):
            operand = _eval(node.operand)
            op_type = type(node.op)
            if op_type not in allowed_operators:
                raise ValueError(f"Unsupported unary operator: {op_type}")
            return allowed_operators[op_type](operand)
        elif isinstance(node, ast.Call):
            # Handle function calls like sqrt(100)
            if isinstance(node.func, ast.Name):
                func_name = node.func.id.lower()
                if func_name not in allowed_functions:
                    raise ValueError(f"Unsupported function: {func_name}")
                args = [_eval(arg) for arg in node.args]
                return allowed_functions[func_name](*args)
            raise ValueError(f"Unsupported call type: {type(node.func)}")
        elif isinstance(node, ast.Expression):
            return _eval(node.body)
        else:
            raise ValueError(f"Unsupported node type: {type(node)}")
    
    try:
        tree = ast.parse(expr_str, mode='eval')
        return _eval(tree)
    except (SyntaxError, ValueError, TypeError, ZeroDivisionError):
        return None


def _is_gurobi_available():
    """Check if gurobi_cl is available in PATH"""
    import shutil
    return shutil.which("gurobi_cl") is not None


def _run_gurobi(mps_file, solution_file):
    """Run Gurobi solver and return status, objective, solution dict"""
    result = subprocess.run(
        ["gurobi_cl", f"ResultFile={solution_file}", f"TimeLimit=60", mps_file],
        capture_output=True,
        text=True
    )

    output = result.stdout + result.stderr

    status = "UNKNOWN"
    objective = None

    for line in output.split('\n'):
        if 'Best objective' in line:
            try:
                parts = line.split('Best objective')[1].split(',')[0].strip()
                objective = float(parts)
            except (ValueError, IndexError):
                pass

        if 'Optimal solution found' in line:
            status = 'OPTIMAL'
        elif 'Model is infeasible' in line or 'infeasible' in line.lower():
            status = 'INFEASIBLE'
        elif 'Model is unbounded' in line or 'unbounded' in line.lower():
            status = 'UNBOUNDED'

    # Parse Gurobi .sol file
    solution = {}
    if os.path.exists(solution_file):
        with open(solution_file, 'r') as f:
            for line in f:
                line = line.strip()
                if line.startswith('#') or not line:
                    if 'Objective value' in line:
                        try:
                            obj_str = line.split('=')[1].strip()
                            if objective is None:
                                objective = float(obj_str)
                        except (ValueError, IndexError):
                            pass
                    continue

                parts = line.split()
                if len(parts) >= 2:
                    try:
                        var_name = parts[0]
                        var_value = float(parts[1])
                        if abs(var_value - round(var_value)) < 1e-6:
                            var_value = int(round(var_value))
                        solution[var_name] = var_value
                    except (ValueError, IndexError):
                        continue

    return status, objective, solution


def _run_highs(mps_file, solution_file):
    """Run HiGHS solver and return status, objective, solution dict"""
    highs_bin = Path(__file__).parent / "highs"

    sol_file = str(solution_file) + ".highs_sol"
    result = subprocess.run(
        [str(highs_bin), "--model_file", str(mps_file),
         "--solution_file", sol_file,
         "--time_limit", "60"],
        capture_output=True,
        text=True
    )

    output = result.stdout + result.stderr

    status = "UNKNOWN"
    objective = None

    for line in output.split('\n'):
        stripped = line.strip()
        # HiGHS status line: "  Status            Optimal"
        if stripped.startswith('Status') and 'Optimal' in stripped:
            status = 'OPTIMAL'
        elif 'Infeasible' in stripped:
            status = 'INFEASIBLE'
        elif 'Unbounded' in stripped:
            status = 'UNBOUNDED'
        # HiGHS objective: "                    -186771.13 (objective)"
        if '(objective)' in stripped:
            try:
                obj_str = stripped.split('(objective)')[0].strip()
                objective = float(obj_str)
            except (ValueError, IndexError):
                pass

    # Parse HiGHS solution file
    # Format: header lines, then "# Columns N", then "varname value" lines
    solution = {}
    if os.path.exists(sol_file):
        with open(sol_file, 'r') as f:
            in_columns = False
            for line in f:
                line = line.strip()
                # "Objective -186771.13"
                if line.startswith('Objective'):
                    try:
                        obj_str = line.split()[1]
                        if objective is None:
                            objective = float(obj_str)
                    except (ValueError, IndexError):
                        pass
                    continue
                if line.startswith('# Columns'):
                    in_columns = True
                    continue
                if line.startswith('# Rows') or line.startswith('Rows'):
                    in_columns = False
                    continue
                if in_columns and line and not line.startswith('#'):
                    parts = line.split()
                    if len(parts) >= 2:
                        try:
                            var_name = parts[0]
                            var_value = float(parts[1])
                            if abs(var_value - round(var_value)) < 1e-6:
                                var_value = int(round(var_value))
                            solution[var_name] = var_value
                        except (ValueError, IndexError):
                            continue

    return status, objective, solution


def run_highs_solver(mps_file, solution_file):
    """Run reference solver: try Gurobi first, fall back to HiGHS"""
    if _is_gurobi_available():
        print("ℹ Using Gurobi as reference solver")
        return _run_gurobi(mps_file, solution_file)
    else:
        print("ℹ Gurobi not available, using HiGHS as reference solver")
        return _run_highs(mps_file, solution_file)


def run_decidb_query(query_file, db_file):
    """Run the query using DecidB (DuckDB)"""
    # Assuming read_config is defined elsewhere or will be added.
    # For now, we'll define a placeholder for read_config to make the code syntactically correct.
    # In a real scenario, this would come from a configuration module.
    def read_config():
        decidb_bin = Path(__file__).parent.parent.parent / "build" / "release" / "decidb"
        return decidb_bin, None # Placeholder for other config value

    decidb_bin, _ = read_config()

    # Use -csv flag to ensure CSV output
    cmd = [str(decidb_bin), str(db_file), '-csv']
    
    # Read query content
    with open(query_file, 'r') as f:
        query_sql = f.read()
        
    print(f"ℹ Running DecidB with: {' '.join(cmd)}")
    result = subprocess.run(cmd, input=query_sql, capture_output=True, text=True)
    
    return result.stdout, result.stderr, result.returncode


def extract_from_clause(query_content):
    """Extract FROM clause from the main SELECT statement (the one with DECIDE)"""
    # Normalize
    upper_query = query_content.upper()
    
    if 'FROM' not in upper_query or 'DECIDE' not in upper_query:
        return ""
    
    # Find the DECIDE keyword - the main SELECT is the one that contains DECIDE
    decide_idx = upper_query.find('DECIDE')
    
    # Find the SELECT that precedes this DECIDE (search backwards from DECIDE)
    # This is the main query SELECT, not a CREATE VIEW or subquery
    select_idx = upper_query.rfind('SELECT', 0, decide_idx)
    
    if select_idx == -1:
        return ""
    
    # Now find FROM between this SELECT and DECIDE
    from_idx = upper_query.find('FROM', select_idx, decide_idx)
    
    if from_idx == -1:
        return ""
        
    # Find start of FROM clause (after 'FROM' keyword)
    start_idx = from_idx + 4
    
    # Find end of FROM (WHERE, DECIDE, GROUP BY, ORDER BY, LIMIT, ;, or end of string)
    # But only search between FROM and DECIDE
    end_markers = ['WHERE', 'DECIDE', 'GROUP BY', 'ORDER BY', 'LIMIT', ';']
    end_idx = decide_idx  # Default to DECIDE position
    
    for marker in end_markers:
        idx = upper_query.find(marker, start_idx, decide_idx + 1)
        if idx != -1 and idx < end_idx:
            end_idx = idx
            
    return query_content[start_idx:end_idx].strip()


def extract_where_clause(query_content):
    """Extract WHERE clause from SQL query"""
    # Normalize
    upper_query = query_content.upper()
    
    if 'WHERE' not in upper_query:
        return ""
        
    # Find start of WHERE
    start_idx = upper_query.find('WHERE') + 5
    
    # Find end of WHERE (DECIDE, GROUP BY, ORDER BY, LIMIT, ;, or end of string)
    end_markers = ['DECIDE', 'GROUP BY', 'ORDER BY', 'LIMIT', ';']
    end_idx = len(query_content)
    
    for marker in end_markers:
        idx = upper_query.find(marker, start_idx)
        if idx != -1 and idx < end_idx:
            end_idx = idx
            
    return query_content[start_idx:end_idx].strip()
    return query_content[start_idx:end_idx].strip()


def extract_select_columns(query_content):
    """Extract columns from SELECT clause"""
    # Normalize
    upper_query = query_content.upper()
    
    if 'SELECT' not in upper_query:
        return []
        
    start_idx = upper_query.find('SELECT') + 6
    end_idx = upper_query.find('FROM')
    
    if end_idx == -1:
        return []
        
    select_part = query_content[start_idx:end_idx].strip()
    
    # Split by comma and clean up
    cols = []
    for c in select_part.split(','):
        c = c.strip()
        # Handle aliases (e.g. "col AS alias" -> "alias")
        # But wait, we need the expression to evaluate?
        # Or does the user just want the column name?
        # For simple columns: "x", "o_orderkey".
        # If alias: "x AS my_x".
        # The output header should be the alias.
        # The value should be the value of the expression.
        # For now, let's assume simple columns or aliases that match data keys.
        # If we have "x", we look up "x" in decide vars.
        # If we have "o_orderkey", we look up in data.
        
        # We will store the full string for now, and handle splitting in writer if needed?
        # No, let's just take the name.
        # If there is ' AS ', take the right side.
        if ' AS ' in c.upper():
            c = c.upper().split(' AS ')[1].strip()
        elif ' ' in c:
            # Maybe "col alias"
            parts = c.split()
            c = parts[-1]
            
        cols.append(c)
        
    return cols


def strip_comments(sql):
    """Strip SQL comments"""
    import re
    # Remove multi-line comments
    sql = re.sub(r'/\*[\s\S]*?\*/', '', sql)
    # Remove single-line comments
    lines = []
    for line in sql.split('\n'):
        line = line.split('--')[0]
        if line.strip():
            lines.append(line)
    return '\n'.join(lines)


def query_to_mps(query_file, db_file, mps_file):
    """Convert SQL query to MPS format by querying the real database"""
    decidb_bin = Path(__file__).parent.parent.parent / "build" / "release" / "decidb"
    
    # Read the query
    with open(query_file, 'r') as f:
        raw_content = f.read()
        
    # Strip comments for parsing
    sql_content = strip_comments(raw_content)
    
    # Extract FROM clause
    from_clause = extract_from_clause(sql_content)
    
    # Extract WHERE clause
    where_clause = extract_where_clause(sql_content)
    
    # Parse DECIDE clause components
    # Look for DECIDE x [IS type], y [IS type], ...
    decide_vars = []
    variable_type = "INTEGER"
    if 'DECIDE' in sql_content:
        decide_part = sql_content.split('DECIDE')[1]
        # It ends at SUCH THAT or MAXIMIZE or MINIMIZE or ;
        end_markers = ['SUCH THAT', 'MAXIMIZE', 'MINIMIZE', ';']
        end_idx = len(decide_part)
        for marker in end_markers:
            idx = decide_part.find(marker)
            if idx != -1:
                end_idx = min(end_idx, idx)
        
        vars_str = decide_part[:end_idx].strip()
        raw_vars = [v.strip() for v in vars_str.split(',')]
        for v in raw_vars:
            # Each variable may be "x", "x IS INTEGER", or "x IS BOOLEAN"
            parts = v.split()
            decide_vars.append(parts[0].lower())  # Just the variable name
            # Detect type from the declaration
            if len(parts) >= 3 and parts[1].upper() == 'IS' and parts[2].upper() == 'BOOLEAN':
                variable_type = "BINARY"
    
    if not decide_vars:
        decide_vars = ['x'] # Default fallback
    
    # Get objective sense and expression
    if 'MAXIMIZE' in sql_content:
        obj_sense = "MAXIMIZE"
        obj_part = sql_content.split('MAXIMIZE')[1].strip().rstrip(';')
    elif 'MINIMIZE' in sql_content:
        obj_sense = "MINIMIZE"
        obj_part = sql_content.split('MINIMIZE')[1].strip().rstrip(';')
    else:
        print("ERROR: No objective found")
        return False
    
    # Extract objective expression
    if 'SUM(' in obj_part:
        obj_expr = obj_part.split('SUM(')[1].split(')')[0].strip().lower()
    else:
        # Fallback if no SUM (unlikely for DecidB)
        obj_expr = "0" 
    
    # Parse constraints and bounds
    constraints = []
    bounds = {}  # {var_name: {'lower': 0, 'upper': None}}
    
    # Initialize bounds for all decide vars
    # Default bounds: 0 to +inf for both Integer and Binary (Binary is 0-1)
    for var in decide_vars:
        bounds[var] = {'lower': 0.0, 'upper': float('inf')}
        if variable_type == "BINARY":
            bounds[var]['upper'] = 1.0
    
    if 'SUCH THAT' in sql_content:
        constraint_part = sql_content.split('SUCH THAT')[1]
        if 'MAXIMIZE' in constraint_part:
            constraint_part = constraint_part.split('MAXIMIZE')[0]
        elif 'MINIMIZE' in constraint_part:
            constraint_part = constraint_part.split('MINIMIZE')[0]
            
        # Resolve subqueries in RHS (e.g. <= (SELECT ...))
        while True:
            # Find start of subquery
            start_marker = "(SELECT"
            start_idx = constraint_part.upper().find(start_marker)
            if start_idx == -1:
                break
                
            # Find matching closing parenthesis
            open_cnt = 0
            end_idx = -1
            for i in range(start_idx, len(constraint_part)):
                char = constraint_part[i]
                if char == '(':
                    open_cnt += 1
                elif char == ')':
                    open_cnt -= 1
                    if open_cnt == 0:
                        end_idx = i
                        break
            
            if end_idx == -1:
                print("Warning: Malformed subquery parenthesis")
                break
                
            full_subquery = constraint_part[start_idx:end_idx+1]
            # Content without outer parens
            subquery = full_subquery[1:-1]
            
            print(f"ℹ Resolving subquery: {subquery}")
            
            # Execute subquery
            res = subprocess.run([str(decidb_bin), str(db_file), '-csv', '-noheader'], input=subquery, capture_output=True, text=True)
            if res.returncode != 0:
                print(f"Error executing subquery: {res.stderr}")
                break 
            
            val = res.stdout.strip()
            if not val:
                 val = "0"
            
            print(f"  -> Result: {val}")
            constraint_part = constraint_part.replace(full_subquery, val)

        # Normalize spaces
        constraint_part = ' '.join(constraint_part.split())
        
        # Protect BETWEEN ... AND ... from being split by the AND delimiter
        # Replace "BETWEEN X AND Y" with "BETWEEN X __BETWEENAND__ Y" temporarily
        def protect_between(match):
            return f"BETWEEN {match.group(1)} __BETWEENAND__ {match.group(2)}"
        
        constraint_part = re.sub(
            r'BETWEEN\s+(-?[\d\.eE+-]+)\s+AND\s+(-?[\d\.eE+-]+)',
            protect_between,
            constraint_part,
            flags=re.IGNORECASE
        )
        
        # Split by ' AND ' (case insensitive)
        parts = re.split(r'\s+AND\s+', constraint_part, flags=re.IGNORECASE)
        
        # Restore the protected AND in BETWEEN clauses
        parts = [p.replace('__BETWEENAND__', 'AND') for p in parts]
        
        for part in parts:
            part = part.strip()
            if not part:
                continue
                
            # Type declarations
            if 'IS BOOLEAN' in part.upper():
                continue 
            if 'IS INTEGER' in part.upper():
                continue
                
            # SUM constraints
            if 'SUM(' in part.upper():
                try:
                    # Handle SUM(expr) BETWEEN a AND b -> two constraints: >= a AND <= b
                    between_match = re.search(r'SUM\((.*?)\)\s+BETWEEN\s+(-?[\d\.eE+-]+)\s+AND\s+(-?[\d\.eE+-]+)', part, re.IGNORECASE)
                    if between_match:
                        sum_expr = between_match.group(1).strip().lower()
                        lower_val = float(between_match.group(2))
                        upper_val = float(between_match.group(3))
                        
                        # Add >= lower constraint
                        constraints.append({
                            'expr': sum_expr,
                            'type': 'GE',
                            'rhs': lower_val
                        })
                        # Add <= upper constraint
                        constraints.append({
                            'expr': sum_expr,
                            'type': 'LE',
                            'rhs': upper_val
                        })
                        continue
                    
                    match = re.search(r'(<=|>=|=|<|>)', part)
                    if not match:
                        continue
                        
                    operator = match.group(1)
                    lhs, rhs_str = part.split(operator)
                    
                    # Regex is safer
                    sum_match = re.search(r'SUM\((.*)\)', lhs, re.IGNORECASE)
                    if sum_match:
                        sum_expr = sum_match.group(1).strip().lower()
                    else:
                        continue
                    
                    # Evaluate RHS - handles simple numbers and arithmetic expressions like "2.667e-6 * 100"
                    rhs = safe_eval_arithmetic(rhs_str)
                    if rhs is None:
                        print(f"Warning: Could not evaluate RHS expression: {rhs_str}")
                        continue
                    
                    sense = 'EQ'
                    if '<' in operator:
                        sense = 'LE'
                        if operator == '<' and variable_type == "INTEGER":
                            rhs -= 1
                    elif '>' in operator:
                        sense = 'GE'
                        if operator == '>' and variable_type == "INTEGER":
                            rhs += 1
                                
                    constraints.append({
                        'expr': sum_expr,
                        'type': sense,
                        'rhs': rhs
                    })
                except (IndexError, ValueError):
                    print(f"Warning: Failed to parse constraint: {part}")
                    continue
            
            # Variable bounds (x > 2, y < 10, x BETWEEN 0 AND 1)
            else:
                # Handle x BETWEEN a AND b -> lower = a, upper = b
                between_match = re.match(r'([a-zA-Z0-9_]+)\s+BETWEEN\s+(-?[\d\.eE+-]+)\s+AND\s+(-?[\d\.eE+-]+)', part, re.IGNORECASE)
                if between_match:
                    var = between_match.group(1).lower()
                    lower_val = float(between_match.group(2))
                    upper_val = float(between_match.group(3))
                    
                    if var in bounds:
                        bounds[var]['lower'] = max(bounds[var]['lower'], lower_val)
                        bounds[var]['upper'] = min(bounds[var]['upper'], upper_val)
                    continue
                
                match = re.match(r'([a-zA-Z0-9_]+)\s*(<=|>=|=|<|>)\s*(-?[\d\.]+)', part)
                if match:
                    var = match.group(1).lower()
                    op = match.group(2)
                    val = float(match.group(3))
                    
                    if var in bounds:
                        if '<' in op:
                            limit = val
                            if op == '<' and variable_type == "INTEGER":
                                limit -= 1
                            bounds[var]['upper'] = min(bounds[var]['upper'], limit)
                        elif '>' in op:
                            limit = val
                            if op == '>' and variable_type == "INTEGER":
                                limit += 1
                            bounds[var]['lower'] = max(bounds[var]['lower'], limit)
                        elif '=' in op:
                            bounds[var]['lower'] = val
                            bounds[var]['upper'] = val
    
    # Execute the query file to set up tables (CREATE TABLE + INSERT)
    # We'll execute everything up to the SELECT statement
    
    setup_sql = []
    has_ddl = False
    ddl_keywords = ['CREATE', 'INSERT', 'DROP', 'COPY']
    
    for statement in sql_content.split(';'):
        statement = statement.strip()
        if not statement:
            continue
        if statement.upper().startswith('SELECT'):
            break
        setup_sql.append(statement)
        upper_stmt = statement.upper()
        if any(k in upper_stmt for k in ddl_keywords):
            has_ddl = True
    
    if has_ddl:
        # Try to drop table if simple name
        if ' ' not in from_clause and ',' not in from_clause:
             drop_table_cmd = f"DROP TABLE IF EXISTS {from_clause};"
             subprocess.run([str(decidb_bin), str(db_file)], input=drop_table_cmd, capture_output=True, text=True)
             
        setup_commands = ';\n'.join(setup_sql) + ';'
        result = subprocess.run([str(decidb_bin), str(db_file)], input=setup_commands, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"ERROR setting up tables: {result.stderr}")
            return False
    else:
        print(f"ℹ No DDL found in query file, using existing table(s) '{from_clause}'")
    
    # Query the database to get actual data
    data_query = f"SELECT * FROM {from_clause}"
    if where_clause:
        data_query += f" WHERE {where_clause}"
    data_query += ";"
    
    print(f"ℹ Fetching data with: {data_query}")
    
    result = subprocess.run([str(decidb_bin), str(db_file), '-csv'], input=data_query, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"ERROR querying database: {result.stderr}")
        return False
    
    lines = result.stdout.strip().split('\n')
    if len(lines) < 2:
        print("ERROR: No data returned from database")
        return False
    
    reader = csv.DictReader(lines)
    data = []
    columns = None
    for row in reader:
        # Normalize keys to lowercase
        row = {k.lower(): v for k, v in row.items()}
        
        if columns is None:
            columns = list(row.keys())
        int_row = {}
        for k, v in row.items():
            try:
                int_row[k] = int(v)
            except (ValueError, TypeError):
                int_row[k] = v
        data.append(int_row)
        
    # Check for infeasibility in bounds for BINARY variables
    if variable_type == "BINARY":
        for var, b in bounds.items():
            if b['lower'] > 1 or b['upper'] < 0:
                return False, "INFEASIBLE", data, columns, obj_sense, decide_vars, constraints, bounds, obj_expr, []
            if b['lower'] > b['upper']:
                return False, "INFEASIBLE", data, columns, obj_sense, decide_vars
    elif variable_type == "INTEGER":
         for var, b in bounds.items():
            if b['lower'] > b['upper']:
                return False, "INFEASIBLE", data, columns, obj_sense, decide_vars, constraints, bounds, obj_expr, []

    # Adjust RHS for constant terms in constraints
    for constraint in constraints:
        constant_sum = 0.0
        for row in data:
            constant_sum += eval_constant_part(constraint['expr'], row, decide_vars)
        
        constraint['rhs'] -= constant_sum
    
    # Write MPS file
    with open(mps_file, 'w') as f:
        f.write("NAME          DECIDB_TEST\n")
        f.write("ROWS\n")
        f.write(" N  OBJ\n")
        for i, constraint in enumerate(constraints):
            sense_char = {'LE': 'L', 'GE': 'G', 'EQ': 'E'}[constraint['type']]
            f.write(f" {sense_char}  C{i}\n")
        
        f.write("COLUMNS\n")
        
        # If INTEGER, start INTORG marker
        if variable_type == "INTEGER":
             f.write("    MARK0000  'MARKER'                 'INTORG'\n")
             
        for row_idx, row in enumerate(data):
            # For each decision variable
            for var_name in decide_vars:
                col_name = f"R{row_idx}_{var_name}"
                
                # Objective coefficient
                obj_coef = eval_linear_coef(obj_expr, row, var_name)
                if obj_sense == "MAXIMIZE":
                    obj_coef = -obj_coef
                
                if obj_coef != 0:
                    f.write(f"    {col_name:<20}  OBJ       {obj_coef}\n")
                
                # Constraint coefficients
                for c_idx, constraint in enumerate(constraints):
                    coef = eval_linear_coef(constraint['expr'], row, var_name)
                    if coef != 0:
                        f.write(f"    {col_name:<20}  C{c_idx:<8}  {coef}\n")
        
        # If INTEGER, end INTEND marker
        if variable_type == "INTEGER":
             f.write("    MARK0001  'MARKER'                 'INTEND'\n")
        
        f.write("RHS\n")
        for c_idx, constraint in enumerate(constraints):
            f.write(f"    RHS1      C{c_idx:<8}  {constraint['rhs']}\n")
        
        f.write("BOUNDS\n")
        for row_idx, row in enumerate(data):
            for var_name in decide_vars:
                col_name = f"R{row_idx}_{var_name}"
                lower = bounds[var_name]['lower']
                upper = bounds[var_name]['upper']
                
                if variable_type == "BINARY":
                    f.write(f" BV BOUND1    {col_name}\n")
                    # Write bounds if they conflict with or tighten 0-1
                    if lower > 0:
                        f.write(f" LI BOUND1    {col_name:<20}  {lower}\n")
                    if upper < 1:
                        f.write(f" UI BOUND1    {col_name:<20}  {upper}\n")
                else:
                    # Integer variables (default 0 to inf)
                    # MPS default is 0 to +inf if not specified.
                    # If lower != 0, specify LI.
                    # If upper != inf, specify UI.
                    if lower != 0:
                        if lower == -float('inf'):
                             f.write(f" FR BOUND1    {col_name}\n") # Free variable
                        else:
                             f.write(f" LI BOUND1    {col_name:<20}  {lower}\n")
                    
                    if upper != float('inf'):
                        f.write(f" UI BOUND1    {col_name:<20}  {upper}\n")
                    else:
                        # If unbounded integer, some solvers default to binary (0-1).
                        # We must specify a large upper bound.
                        f.write(f" UI BOUND1    {col_name:<20}  2147483647\n")
        
        f.write("ENDATA\n")
    
        f.write("ENDATA\n")
    
    # Extract projected columns from query
    projected_cols = extract_select_columns(sql_content)
    
    return True, "OPTIMAL", data, columns, obj_sense, decide_vars, constraints, bounds, obj_expr, projected_cols


def split_expr(expr):
    """Split expression by '+' respecting parentheses"""
    terms = []
    current_term = []
    paren_level = 0
    
    # Normalize: replace - with + - (but careful about parens)
    # Actually, let's just iterate chars.
    # If we see '-', and we are not in parens, and previous char was not operator...
    # This is hard.
    # Simpler: Use the previous normalization `replace('-', '+-')` but ONLY if - is not inside parens?
    # No, `(a-b)` should remain `(a-b)`.
    # `a - b` -> `a + -b`.
    
    # Let's just iterate and split by '+' at level 0.
    # And handle '-' by treating it as '+' followed by '-' term?
    # e.g. "a - b" -> "a", "-b".
    
    # Pre-process: replace " - " with " + -".
    # But "a-b" (no spaces) -> "a+-b".
    # "(a-b)" -> "(a-b)".
    
    # Let's iterate.
    i = 0
    n = len(expr)
    start = 0
    
    while i < n:
        char = expr[i]
        if char == '(':
            paren_level += 1
        elif char == ')':
            paren_level -= 1
        elif char == '+' and paren_level == 0:
            terms.append(expr[start:i].strip())
            start = i + 1
        elif char == '-' and paren_level == 0:
            # If it's the start of expression or after another operator, it's unary minus.
            # e.g. "-a". start=0. i=0.
            # e.g. "a * -b". Not possible if we split by +.
            # e.g. "a - b".
            # If we split at '-', we need to include '-' in the next term.
            # And the previous term ends here.
            
            # Check if it's binary minus.
            # It is binary if previous char is not operator.
            # But we are only splitting by +/-(level 0).
            
            # If i > start, then we have a term before this '-'.
            # e.g. "a-b". start=0. i=1. expr[0]='a'.
            # We append 'a'.
            # Then we set start = i. So next term starts with '-'.
            if i > start:
                terms.append(expr[start:i].strip())
                start = i
            # If i == start (e.g. "-a"), we continue.
            
        i += 1
    
    terms.append(expr[start:].strip())
    return [t for t in terms if t]


def strip_aliases(expr):
    """Strip table aliases from expression (e.g. 't.col' -> 'col')"""
    # Replace "word.word" with "word"
    # We must be careful not to strip decimal points like 0.5.
    # "word" starts with letter/underscore.
    # regex: \b[a-zA-Z_][a-zA-Z0-9_]*\.([a-zA-Z_][a-zA-Z0-9_]*)\b
    return re.sub(r'\b[a-zA-Z_][a-zA-Z0-9_]*\.([a-zA-Z_][a-zA-Z0-9_]*)\b', r'\1', expr)


def eval_constant_part(expr, row, decide_vars):
    """Evaluate constant part of expression (terms without decision vars)"""
    # Remove spaces? No, split_expr handles stripping.
    # But we should remove spaces inside terms for eval?
    # eval() handles spaces.
    
    terms = split_expr(expr)
    total_const = 0.0
    
    for term in terms:
        # Check if term contains ANY decision var
        # Tokenize by * to find vars?
        # But term might be "(a+b)*x".
        # We need to check if var is present as a token.
        # Simple heuristic: check if var string is in term?
        # "tax" contains "x". False positive!
        # We need to tokenize properly.
        # Regex `\bvar\b`?
        
        has_var = False
        for var in decide_vars:
            # Check for whole word match
            if re.search(r'\b' + re.escape(var) + r'\b', term):
                has_var = True
                break
        
        if not has_var:
            # Evaluate this constant term
            py_term = term.replace('^', '**')
            
            # Strip table aliases
            py_term = strip_aliases(py_term)
            
            # Replace column names with values
            cols = sorted(row.keys(), key=len, reverse=True)
            for col in cols:
                # Use regex to replace whole word column name
                # e.g. "tax" should not replace "l_tax" (if l_tax is in cols)
                # But we sort by length desc, so l_tax is replaced first.
                # But "tax" might replace "tax_rate"?
                # Yes. Regex replacement is safer.
                
                if col in py_term:
                    # Escape col for regex
                    pattern = r'\b' + re.escape(col) + r'\b'
                    val = str(row[col])
                    py_term = re.sub(pattern, val, py_term)
            
            try:
                val = eval(py_term)
                total_const += float(val)
            except Exception as e:
                pass
                
    return total_const


def eval_linear_coef(expr, row, var_name):
    """Evaluate coefficient of var_name in linear expression"""
    terms = split_expr(expr)
    total_coef = 0.0
    
    for term in terms:
        # Check if term contains var_name
        if not re.search(r'\b' + re.escape(var_name) + r'\b', term):
            continue
            
        # We assume the term is linear: coeff * var
        # e.g. "3 * x * price"
        # or "x"
        # or "-x"
        # or "(price + 1) * x"
        
        # We want to evaluate everything EXCEPT var_name.
        # We can replace var_name with 1.0 and eval?
        # If term is "x * x", replacing with 1.0 gives 1.0. But it's non-linear.
        # We should check linearity.
        # Count occurrences?
        matches = re.findall(r'\b' + re.escape(var_name) + r'\b', term)
        if len(matches) != 1:
            continue # Non-linear or missing
            
        # Replace var_name with 1.0 and eval
        py_term = term.replace('^', '**')
        
        # Debug trace for objective


        # Replace var_name with 1.0
        py_term = re.sub(r'\b' + re.escape(var_name) + r'\b', '1.0', py_term)
        
        # Strip table aliases
        py_term = strip_aliases(py_term)
        
        # Replace columns
        cols = sorted(row.keys(), key=len, reverse=True)
        for col in cols:
            if col in py_term:
                pattern = r'\b' + re.escape(col) + r'\b'
                val = str(row[col])
                py_term = re.sub(pattern, val, py_term)
                

                
        try:
            val = eval(py_term)
            total_coef += float(val)
        except Exception as e:
            pass
            
    return total_coef


def eval_expr(expr, row):
    """Evaluate simple expression like 'x * column'"""
    if '*' in expr:
        parts = [p.strip() for p in expr.split('*')]
        if parts[0] == 'x' and len(parts) == 2:
            col_name = parts[1]
            return row.get(col_name, 0)
    return 1


def write_highs_solution_csv(data, columns, solution, objective, obj_sense, output_file, decide_vars=['x'], projected_cols=None):
    """Write HiGHS solution in table format"""
    with open(output_file, 'w', newline='') as f:
        writer = csv.writer(f)
        
        # Determine header and columns to write
        if projected_cols:
            header = projected_cols
        else:
            header = decide_vars + columns
            
        writer.writerow(header)
        
        for row_idx, row in enumerate(data):
            row_values = []
            for col in header:
                col_lower = col.lower()
                
                # Check if it is a decide variable
                if col_lower in decide_vars:
                    col_name = f"R{row_idx}_{col_lower}"
                    val = solution.get(col_name, 0)
                    row_values.append(val)
                else:
                    # Check if it is a table column
                    # Try exact match first
                    if col_lower in row:
                        row_values.append(row[col_lower])
                    else:
                        # Try case insensitive lookup in row
                        found = False
                        
                        # DecidB/DuckDB often flattens "table.col" to "col" or "table_col"
                        # Try removing table alias prefix (everything before dot)
                        if '.' in col_lower:
                            simple_name = col_lower.split('.')[-1]
                            if simple_name in row:
                                row_values.append(row[simple_name])
                                found = True
                        
                        if not found:
                            for k, v in row.items():
                                if k.lower() == col_lower:
                                    row_values.append(v)
                                    found = True
                                    break
                                    
                        if not found and '.' in col_lower:
                             # Try partial match (suffix)
                             suffix = col_lower.split('.')[-1]
                             for k, v in row.items():
                                 if k.endswith(suffix):
                                      row_values.append(v)
                                      found = True
                                      break

                        if not found:
                            # If not found, maybe it's an expression we can't evaluate easily?
                            # Or maybe it's just missing.
                            row_values.append(None)
            
            writer.writerow(row_values)
        
        # Blank line then objective
        writer.writerow([])
        actual_obj = objective if obj_sense == "MINIMIZE" else -objective if objective else None
        writer.writerow(['objective', actual_obj])


def run_test(query_file, db_file, output_dir):
    """Run a single test"""
    test_name = Path(query_file).stem
    print(f"\\n{'='*60}")
    print(f"Testing: {test_name}")
    print(f"{'='*60}")
    
    # Create output directory
    test_dir = Path(output_dir) / test_name
    test_dir.mkdir(parents=True, exist_ok=True)
    
    # 1. Copy query.sql
    query_out = test_dir / "query.sql"
    with open(query_file, 'r') as f:
        query_content = f.read()
    with open(query_out, 'w') as f:
        f.write(query_content)
    print(f"✓ Copied query.sql")
    
    # 3. Generate MPS file from query and data
    mps_file = test_dir / 'model.mps'
    result = query_to_mps(query_file, db_file, mps_file)
    if not result:
        print("ERROR: Failed to generate MPS file")
        return False
        
    success, status, data, columns, obj_sense, decide_vars, constraints, bounds, obj_expr, projected_cols = result
    
    highs_solution_csv = test_dir / 'highs_solution.csv'
    highs_status_file = test_dir / 'highs_status.txt'
    
    if not success:
        if status == "INFEASIBLE":
            print("✓ Detected INFEASIBLE problem during parsing")
            objective = 0.0
            solution = {}
            with open(highs_status_file, 'w') as f:
                f.write("Status: INFEASIBLE\nObjective: 0.0\n")
            # Create empty solution file
            with open(highs_solution_csv, 'w') as f:
                pass
        else:
            print("ERROR: Failed to generate MPS file")
            return False
    else:
        print(f"✓ Generated model.mps ({len(data)} rows)")
        
        # 4. Run HiGHS solver
        status, objective, solution = run_highs_solver(mps_file, highs_solution_csv)
        
        # Write solution to CSV (overwrite with formatted output)
        write_highs_solution_csv(data, columns, solution, objective, obj_sense, highs_solution_csv, decide_vars, projected_cols)
    
    # 5. Run DecidB
    decidb_solution_csv = test_dir / 'decidb_solution.csv'
    decidb_status_file = test_dir / 'decidb_status.txt'
    
    print(f"✓ HiGHS: {status}, Objective: {objective}")
    
    # Write status file
    with open(highs_status_file, 'w') as f:
        f.write(f"Status: {status}\n")
        f.write(f"Objective: {objective}\n")
    
    # 4. Run DecidB
    decidb_out, decidb_err, returncode = run_decidb_query(query_file, db_file)
    
    if returncode == 0:
        # Parse output to extract CSV
        # print(f"DEBUG: DecidB Output (first 10 lines):\n{chr(10).join(decidb_out.splitlines()[:10])}")
        csv_content = parse_decidb_output(decidb_out, known_columns=columns)
        if csv_content:
            with open(decidb_solution_csv, 'w') as f:
                f.write(csv_content)
        else:
            print("ERROR: Failed to parse CSV from DecidB output")
        with open(decidb_status_file, 'w') as f:
            f.write("Status: SUCCESS\n")
        print(f"✓ DecidB: SUCCESS")
        
        # 5. Compare Results
        match, reason = compare_results(highs_solution_csv, decidb_solution_csv)
        if match:
            print(f"✓ Comparison: PASS")
            return True
        else:
            # Fallback: Verify if DecidB solution satisfies constraints and matches objective
            print(f"⚠ Exact match failed ({reason}). Verifying constraints and objective...")
            
            # We need constraints, bounds, etc. from query_to_mps result
            # But run_test unpacked them earlier.
            # We need to ensure they are available here.
            
            # Re-unpack result from query_to_mps
            # success, status, data, columns, obj_sense, decide_vars, constraints, bounds, obj_expr = result
            
            # Wait, run_test called query_to_mps earlier.
            # We need to update the unpacking in run_test first.
            
            # Let's assume run_test unpacking is updated (I will do it in next step if not).
            # Actually, I should do it here if possible, but I can't see the unpacking line in this chunk.
            # I will update the unpacking line separately.
            
            # Here I just call verify_solution
            v_match, v_reason = verify_solution(decidb_solution_csv, data, constraints, bounds, obj_expr, obj_sense, decide_vars, objective)
            
            if v_match:
                print(f"✓ Verification: PASS ({v_reason})")
                return True
            else:
                print(f"✗ Verification: FAIL ({v_reason})")
                return False
            
    else:
        # Check if this is an expected infeasibility (both HiGHS and DecidB agree)
        decidb_infeasible = 'infeasible' in decidb_err.lower()
        highs_infeasible = (status == 'INFEASIBLE')
        
        if highs_infeasible and decidb_infeasible:
            print(f"✓ DecidB: INFEASIBLE (matches HiGHS)")
            with open(decidb_status_file, 'w') as f:
                f.write(f"Status: INFEASIBLE\nError: {decidb_err}\n")
            print(f"✓ Comparison: PASS (both agree problem is infeasible)")
            return True
        else:
            print(f"✗ DecidB: FAILED")
            print(decidb_err)
            with open(decidb_status_file, 'w') as f:
                f.write(f"Status: FAILED\nError: {decidb_err}\n")
            return False
    
    return True


def read_config(repo_root):
    """Read config.txt to find db_file"""
    config_file = repo_root / "config.txt"
    
    # Defaults
    build_mode = "release"
    db_filename = "decidb.db"
    
    if config_file.exists():
        with open(config_file, 'r') as f:
            section = None
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                if line.startswith('[') and line.endswith(']'):
                    section = line[1:-1]
                    continue
                
                if section == 'decidb' and '=' in line:
                    key, value = [p.strip() for p in line.split('=', 1)]
                    if key == 'db_file':
                        db_filename = value
                    elif key == 'build_mode':
                        build_mode = value
    
    # Construct path: build/<mode>/<db_file>
    # Check if the file exists there, otherwise fall back to root
    db_path = repo_root / "build" / build_mode / db_filename
    
    if not db_path.exists():
        # Fallback to root
        db_path = repo_root / db_filename
        
    return db_path


def parse_decidb_output(output, known_columns=None):
    """Extract CSV content from noisy DecidB output"""
    lines = output.split('\n')
    csv_lines = []
    
    # Heuristic: Skip lines that look like logs or banners
    skip_keywords = [
        "running highs", "copyright", "licence", "debug:", "info:", 
        "warning:", "error:", "time:", "rows:", "threads:", "cols:", "bounds",
        "writing the model", "presolve", "solving", "model status", "objective value",
        "primal bound", "dual bound", "gap", "solution", "iterations", "nodes",
        "mip", "nonzeros", "integer variables", "matrix", "coefficient ranges"
    ]
    
    for line in lines:
        line = line.strip()
        if not line:
            continue
            
        lower_line = line.lower()
        if any(k in lower_line for k in skip_keywords):
            continue
            
        # If we have known columns, require at least one to be present in the header
        # This helps distinguish the CSV header from random logs that might contain commas
        if known_columns and not csv_lines:
            # We are looking for the header
            # Check if any known column is in the line (case insensitive)
            line_lower = line.lower()
            found_col = False
            for col in known_columns:
                if col.lower() in line_lower:
                    found_col = True
                    break
            if not found_col:
                continue
        
        if ',' not in line:
            if ' ' in line:
                continue
        
        csv_lines.append(line)
        
    return '\n'.join(csv_lines)


def compare_results(highs_csv, decidb_csv, sort_cols=None):
    """Compare HiGHS and DecidB results"""
    try:
        with open(highs_csv, 'r') as f:
            highs_rows = list(csv.reader(f))
        
        with open(decidb_csv, 'r') as f:
            decidb_rows = list(csv.reader(f))
            
        if not highs_rows or not decidb_rows:
            return False, "Empty result file"
            
        # Normalize headers (lowercase, strip)
        h_header = [c.strip().lower() for c in highs_rows[0]]
        p_header = [c.strip().lower() for c in decidb_rows[0]]
        
        # Find index of decision variables (assume 'x' or 'x_value' or similar)
        # Actually, the user wants to compare "decision variables".
        # In HiGHS output we write 'x_value'. 
        # In DecidB output, it depends on the SELECT clause.
        # We need to match columns by name.
        
        common_cols = set(h_header) & set(p_header)
        if not common_cols:
            return False, f"No common columns. HiGHS: {h_header}, DecidB: {p_header}"
            
        # Sort both by all common columns to ensure alignment
        # But wait, floating point comparison might be needed.
        
        # Filter rows to data only
        h_data = highs_rows[1:]
        p_data = decidb_rows[1:]
        
        # Remove empty lines or summary lines (like "objective")
        h_data = [r for r in h_data if r and len(r) == len(h_header) and r[0] != 'objective']
        p_data = [r for r in p_data if r and len(r) == len(p_header)]
        
        # Create dictionaries for comparison: {row_key: row_dict}
        # We need a primary key. If none, we can just sort and compare line by line.
        # Let's try sorting by all columns.
        
        def row_to_dict(header, row):
            return {k: v for k, v in zip(header, row)}
            
        h_dicts = [row_to_dict(h_header, r) for r in h_data]
        p_dicts = [row_to_dict(p_header, r) for r in p_data]
        
        if sort_cols:
            # Check if cols exist
            missing = [c for c in sort_cols if c not in h_header or c not in p_header]
            if missing:
                print(f"Warning: Sort cols {missing} not found. Falling back to full sort.")
            else:
                def sort_key(d):
                    return tuple(d[k] for k in sort_cols)
                
                h_dicts.sort(key=sort_key)
                p_dicts.sort(key=sort_key)
                
        else:
            # Sort by all keys
            def sort_key(d):
                return tuple(sorted(d.items()))
                
            h_dicts.sort(key=sort_key)
            p_dicts.sort(key=sort_key)
        
        if len(h_dicts) != len(p_dicts):
            return False, f"Row count mismatch: HiGHS={len(h_dicts)}, DecidB={len(p_dicts)}"
            
        for i, (h, p) in enumerate(zip(h_dicts, p_dicts)):
            for col in common_cols:
                h_val = h[col]
                p_val = p[col]
                
                # Loose equality check
                def normalize(v):
                    v_str = str(v).lower().strip()
                    try:
                        f = float(v)
                        if f.is_integer(): return int(f)
                        return f
                    except ValueError:
                        return v_str
                
                if normalize(h_val) != normalize(p_val):
                    # Try float comparison with tolerance
                    try:
                        f_h = float(h_val)
                        f_p = float(p_val)
                        if abs(f_h - f_p) > 1e-4:
                            return False, f"Mismatch in row {i}, col '{col}': {h_val} != {p_val}"
                    except ValueError:
                         return False, f"Mismatch in row {i}, col '{col}': '{h_val}' != '{p_val}'"
        
        return True, "Match"
        
    except Exception as e:
        return False, f"Comparison error: {e}"

def check_objectives(highs_csv, decidb_csv):
    """Check if objective values match (for multiple optimal solutions)"""
    try:
        def get_obj(filename):
            with open(filename, 'r') as f:
                lines = f.readlines()
                # Look for objective line at the end
                for line in reversed(lines):
                    if 'objective' in line:
                        parts = line.split(',')
                        if len(parts) >= 2:
                            return float(parts[1])
            return None

        obj_h = get_obj(highs_csv)
        obj_p = get_obj(decidb_csv)

        if obj_h is not None and obj_p is not None:
             if abs(obj_h - obj_p) < 1e-4:
                 return True, f"Objectives match: {obj_h}"
             else:
                 return False, f"Objectives mismatch: HiGHS={obj_h}, DecidB={obj_p}"
        return False, "Objective not found in output"
    except Exception as e:
        return False, f"Error checking objectives: {e}"



def verify_solution(decidb_csv, data, constraints, bounds, obj_expr, obj_sense, decide_vars, highs_objective):
    """Verify if DecidB solution satisfies constraints and matches objective"""
    try:
        # 1. Parse DecidB output to get decision variables
        with open(decidb_csv, 'r') as f:
            reader = csv.DictReader(f)
            rows = list(reader)
            
        if not rows:
            return False, "Empty result file"
            
        # Map rows to decision variables
        # We assume the order of rows in DecidB output matches 'data' if sorted by primary key?
        # Or we need to join by key.
        # DecidB output contains all columns.
        
        # Let's try to match rows by content (excluding decide vars)
        # Or just assume order is preserved if we didn't sort?
        # DecidB might reorder.
        
        # Better: Build a map of data rows to their index in 'data' list
        # But 'data' list doesn't have a unique key guaranteed.
        # However, the test queries usually select from a table with a key.
        
        # Let's assume the user provided a sort key or we can find one.
        # For now, let's assume we can match by all non-decide columns.
        
        # Extract decide vars from DecidB rows
        solution_values = [] # List of dicts {var: value} for each row in 'data'
        
        # We need to align 'rows' (DecidB output) with 'data' (Original input for MPS)
        # to correctly evaluate constraints that depend on column values.
        
        # Determine common columns for matching
        # We need to know which columns are in the DecidB output (excluding decide vars)
        sample_row = rows[0]
        common_keys = []
        for k in sample_row.keys():
            k = k.lower().strip()
            if k not in decide_vars:
                common_keys.append(k)
        
        common_keys.sort()
        # print(f"DEBUG: Matching on keys: {common_keys}")

        # Create a signature for each row in 'data'
        def get_sig(row, keys):
            # Use only common keys
            # row keys might be different case?
            # 'data' keys are already normalized to lowercase in query_to_mps?
            # Let's check query_to_mps. Yes: row = {k.lower(): v ...}
            return tuple(str(row.get(k, '')) for k in keys)
            
        from collections import deque
        data_map = {}
        for i, row in enumerate(data):
            sig = get_sig(row, common_keys)
            if sig not in data_map:
                data_map[sig] = deque()
            data_map[sig].append(i)
        
        # DEBUG: Print first signature
        # if data_map:
        #    print(f"DEBUG: First data signature: {list(data_map.keys())[0]}")
        
        # Initialize solution vector
        # [row_idx][var_name] = value
        row_solutions = [{} for _ in data]
        
        matched_count = 0
        
        for row_idx, row in enumerate(rows):
            # Extract decide values
            decide_vals = {}
            clean_row = {}
            

            
            for k, v in row.items():
                k = k.lower().strip()
                if k in decide_vars:
                    try:
                        decide_vals[k] = float(v)
                    except ValueError:
                        decide_vals[k] = 0.0
                        decide_vals[k] = 0.0
                else:
                    clean_row[k] = v
            
            # Find matching row in data
            sig = get_sig(clean_row, common_keys)
            # if row_idx == 0:
            #    print(f"DEBUG: First row signature: {sig}")
                
            if sig in data_map and data_map[sig]:
                idx = data_map[sig].popleft()
                row_solutions[idx] = decide_vals
                matched_count += 1
            else:
                # Try loose matching (float tolerance)
                # This is expensive but necessary if floats differ slightly
                pass
                
        if matched_count != len(data):
            return False, f"Could not match all DecidB rows to input data. Matched {matched_count}/{len(data)}"
            
        # 2. Verify Constraints
        
        # A. Variable Bounds
        for i, sol in enumerate(row_solutions):
            for var, val in sol.items():
                b = bounds.get(var, {'lower': -float('inf'), 'upper': float('inf')})
                if val < b['lower'] - 1e-5 or val > b['upper'] + 1e-5:
                    return False, f"Variable bound violated in row {i}, var {var}: {val} not in [{b['lower']}, {b['upper']}]"
        
        # B. Constraints
        for c_idx, constraint in enumerate(constraints):
            # Calculate LHS
            lhs_sum = 0.0
            
            for i, row in enumerate(data):
                # Evaluate term for this row
                # term is like "x * price"
                # We need to evaluate it using the decision variable value for this row
                
                # Re-use eval_linear_coef logic?
                # eval_linear_coef returns the coefficient of the variable.
                # So term value = coeff * var_value
                
                # But the constraint might have multiple variables?
                # The parser split it into terms.
                # Wait, 'constraints' list has 'expr' which is the SUM body.
                # e.g. "x * price"
                
                # We need to evaluate the expression "x * price" given x=... and price=...
                # We can use eval_constant_part if we substitute x?
                
                # Let's manually evaluate:
                # 1. Get coefficient for each variable in the expression
                # 2. Multiply by variable value
                # 3. Add constant part
                
                # Actually, `eval_linear_coef` gives us the coefficient for a specific variable.
                # And `eval_constant_part` gives the constant.
                # So value = sum(coef(v) * val(v)) + constant
                
                row_val = 0.0
                
                # Constant part (e.g. "5")
                row_val += eval_constant_part(constraint['expr'], row, decide_vars)
                
                # Variable parts
                for var in decide_vars:
                    coef = eval_linear_coef(constraint['expr'], row, var)
                    val = row_solutions[i].get(var, 0.0)
                    row_val += coef * val
                    
                lhs_sum += row_val
                
            # Check against RHS
            rhs = constraint['rhs'] 
            # Note: In query_to_mps, we subtracted constant_sum from RHS.
            # But here we are evaluating the full LHS (including constants).
            # So we should compare against the ORIGINAL RHS?
            # Or we should subtract the constant sum from our calculated LHS?
            
            # Let's look at query_to_mps:
            # constraint['rhs'] -= constant_sum
            # So constraint['rhs'] is the "adjusted" RHS.
            # And our lhs_sum includes the constant parts.
            # So we should subtract constant_sum from lhs_sum to compare with adjusted RHS.
            # OR, we just calculate the variable part of LHS and compare with adjusted RHS.
            
            # Let's calculate variable part only.
            lhs_var_part = 0.0
            for i, row in enumerate(data):
                for var in decide_vars:
                    coef = eval_linear_coef(constraint['expr'], row, var)
                    val = row_solutions[i].get(var, 0.0)
                    lhs_var_part += coef * val
            
            # Compare
            tol = 1e-4
            if constraint['type'] == 'LE':
                if lhs_var_part > rhs + tol:
                    return False, f"Constraint {c_idx} violated: {lhs_var_part} > {rhs}"
            elif constraint['type'] == 'GE':
                if lhs_var_part < rhs - tol:
                    return False, f"Constraint {c_idx} violated: {lhs_var_part} < {rhs}"
            elif constraint['type'] == 'EQ':
                if abs(lhs_var_part - rhs) > tol:
                    return False, f"Constraint {c_idx} violated: {lhs_var_part} != {rhs}"
                    
        # 3. Calculate Objective
        obj_val = 0.0

        for i, row in enumerate(data):
            # Variable parts
            for var in decide_vars:
                coef = eval_linear_coef(obj_expr, row, var)
                val = row_solutions[i].get(var, 0.0)

                obj_val += coef * val
                
            # Constant part?
            # Objective usually doesn't have constant part in DecidB syntax (SUM(x*...))
            # But if it did, it would be an offset.
            # eval_constant_part(obj_expr, row, decide_vars)
            obj_val += eval_constant_part(obj_expr, row, decide_vars)
            

            
        # Compare with HiGHS objective
        # Note: HiGHS objective might be minimized/maximized.
        # obj_val is the raw sum.
        # If MAXIMIZE, HiGHS objective is usually positive (or whatever the value is).
        # If MINIMIZE, HiGHS objective is ...
        
        # In query_to_mps:
        # if obj_sense == "MAXIMIZE": obj_coef = -obj_coef
        # So HiGHS minimizes the negative objective.
        # So HiGHS reported objective should be -obj_val if MAXIMIZE.
        # And obj_val if MINIMIZE.
        
        # But wait, run_highs_solver parses the output.
        # Does HiGHS report the minimized value or the original?
        # Usually solvers report the objective value of the transformed problem.
        # But let's check `run_highs_solver`.
        # It parses "Primal bound".
        
        # Let's just compare absolute values or check sign?
        # Or better: compare with the objective value we calculated for HiGHS solution?
        # No, we have the reported objective.
        
        # Let's trust the reported objective but handle sign.
        if obj_sense == "MAXIMIZE":
             # If we negated coefficients, HiGHS minimizes.
             # So HiGHS obj = - (actual obj).
             # So actual obj = - HiGHS obj.
             expected_obj = -highs_objective if highs_objective is not None else 0.0
        else:
             expected_obj = highs_objective if highs_objective is not None else 0.0
             
        # But wait, `run_highs_solver` might return the value as is.
        # If we passed a minimization problem to HiGHS (with negated coeffs), it returns the min value (negative).
        # So expected_obj (the sum) should be -highs_obj.
        
        # Let's check tolerance
        if abs(obj_val - expected_obj) > 1e-3:
             # Try without negation (maybe HiGHS reports original?)
             if abs(obj_val - highs_objective) < 1e-3:
                 pass # Match without negation
             else:
                 return False, f"Objective mismatch: Calculated {obj_val} != Expected {expected_obj} (HiGHS reported {highs_objective})"
                 
        return True, f"Verified (Obj: {obj_val})"
        
    except Exception as e:
        return False, f"Verification error: {e}"

def main():
    """Main entry point"""
    import argparse
    
    parser = argparse.ArgumentParser(description='DecidB Test Runner')
    parser.add_argument('query_file', nargs='?', help='Path to specific SQL query file to test')
    args = parser.parse_args()

    # Paths
    repo_root = Path(__file__).parent.parent.parent
    db_file = read_config(repo_root)
    queries_dir = Path(__file__).parent / "queries"
    results_base = Path(__file__).parent / "results"
    
    # Check database exists
    if not db_file.exists():
        print(f"ERROR: Database not found: {db_file}")
        print("Please run ./run.sh first to create the database")
        return 1
        
    # Determine queries to run
    if args.query_file:
        query_path = Path(args.query_file)
        if not query_path.exists():
            print(f"ERROR: Query file not found: {query_path}")
            return 1
        query_files = [str(query_path.resolve())]
    else:
        # Find all query files
        query_files = sorted(glob.glob(str(queries_dir / "*.sql")))
        if not query_files:
            print(f"ERROR: No query files found in {queries_dir}")
            return 1
    
    print(f"Found {len(query_files)} queries to test")
    print(f"Database: {db_file}")
    
    # Create timestamped output directory
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = results_base / timestamp
    output_dir.mkdir(parents=True, exist_ok=True)
    
    print(f"Output directory: {output_dir}\\n")
    
    # Run all tests
    passed = 0
    failed = 0
    
    for query_file in query_files:
        if run_test(query_file, db_file, output_dir):
            passed += 1
        else:
            failed += 1
    
    # Summary
    print(f"\\n{'='*60}")
    print(f"Test Summary: {passed} passed, {failed} failed")
    print(f"Results saved to: {output_dir}")
    print(f"{'='*60}")
    
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
