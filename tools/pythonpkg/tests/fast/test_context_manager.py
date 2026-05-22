import decidb


class TestContextManager(object):
    def test_context_manager(self, duckdb_cursor):
        with decidb.connect(database=':memory:', read_only=False) as con:
            assert con.execute("select 1").fetchall() == [(1,)]
