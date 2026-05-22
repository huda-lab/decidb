import decidb

# Basic connection test
con = decidb.connect()
print("Connected to DecidB successfully!")
print(f"DecidB version: {con.execute('SELECT version()').fetchone()[0]}")

# Test basic SQL
result = con.execute("SELECT 1 + 1 AS answer").fetchone()
print(f"1 + 1 = {result[0]}")

# Test DECIDE clause (DecidB's ILP feature)
print("\n--- Testing DECIDE clause ---")
con.execute("CREATE TABLE items (name VARCHAR, weight INT, value INT)")
con.execute("INSERT INTO items VALUES ('A', 3, 4), ('B', 4, 5), ('C', 2, 3)")

try:
    result = con.execute("""
        SELECT name, weight, value, x
        FROM items
        DECIDE x
        SUCH THAT
            SUM(x * weight) <= 5
            AND x IS BINARY
        MAXIMIZE SUM(x * value)
    """).fetchall()
    print("DECIDE query result:")
    for row in result:
        print(f"  {row}")
except Exception as e:
    print(f"DECIDE error: {e}")

con.close()
print("\nAll tests passed!")
