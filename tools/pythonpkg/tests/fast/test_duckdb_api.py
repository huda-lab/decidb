import decidb
import sys


def test_duckdb_api():
    res = decidb.execute("SELECT name, value FROM duckdb_settings() WHERE name == 'duckdb_api'")
    formatted_python_version = f"{sys.version_info.major}.{sys.version_info.minor}"
    assert res.fetchall() == [('duckdb_api', f'python/{formatted_python_version}')]
