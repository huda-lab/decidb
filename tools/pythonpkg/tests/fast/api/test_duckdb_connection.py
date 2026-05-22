import decidb
import pytest
from conftest import NumpyPandas, ArrowPandas

pa = pytest.importorskip("pyarrow")


def is_dunder_method(method_name: str) -> bool:
    if len(method_name) < 4:
        return False
    if method_name.startswith('_pybind11'):
        return True
    return method_name[:2] == '__' and method_name[:-3:-1] == '__'


@pytest.fixture(scope="session")
def tmp_database(tmp_path_factory):
    database = tmp_path_factory.mktemp("databases", numbered=True) / "tmp.decidb"
    return database


# This file contains tests for DuckDBPyConnection methods,
# wrapped by the 'decidb' module, to execute with the 'default_connection'
class TestDuckDBConnection(object):
    @pytest.mark.parametrize('pandas', [NumpyPandas(), ArrowPandas()])
    def test_append(self, pandas):
        decidb.execute("Create table integers (i integer)")
        df_in = pandas.DataFrame(
            {
                'numbers': [1, 2, 3, 4, 5],
            }
        )
        decidb.append('integers', df_in)
        assert decidb.execute('select count(*) from integers').fetchone()[0] == 5
        # cleanup
        decidb.execute("drop table integers")

    def test_default_connection_from_connect(self):
        decidb.sql('create or replace table connect_default_connect (i integer)')
        con = decidb.connect(':default:')
        con.sql('select i from connect_default_connect')
        decidb.sql('drop table connect_default_connect')
        with pytest.raises(decidb.Error):
            con.sql('select i from connect_default_connect')

        # not allowed with additional options
        with pytest.raises(
            decidb.InvalidInputException, match='Default connection fetching is only allowed without additional options'
        ):
            con = decidb.connect(':default:', read_only=True)

    def test_arrow(self):
        pyarrow = pytest.importorskip("pyarrow")
        decidb.execute("select [1,2,3]")
        result = decidb.arrow()

    def test_begin_commit(self):
        decidb.begin()
        decidb.execute("create table tbl as select 1")
        decidb.commit()
        res = decidb.table("tbl")
        decidb.execute("drop table tbl")

    def test_begin_rollback(self):
        decidb.begin()
        decidb.execute("create table tbl as select 1")
        decidb.rollback()
        with pytest.raises(decidb.CatalogException):
            # Table does not exist
            res = decidb.table("tbl")

    def test_cursor(self):
        decidb.execute("create table tbl as select 3")
        duckdb_cursor = decidb.cursor()
        res = duckdb_cursor.table("tbl").fetchall()
        assert res == [(3,)]
        duckdb_cursor.execute("drop table tbl")
        with pytest.raises(decidb.CatalogException):
            # 'tbl' no longer exists
            decidb.table("tbl")

    def test_cursor_lifetime(self):
        con = decidb.connect()

        def use_cursors():
            cursors = []
            for _ in range(10):
                cursors.append(con.cursor())

            for cursor in cursors:
                print("closing cursor")
                cursor.close()

        use_cursors()
        con.close()

    def test_df(self):
        ref = [([1, 2, 3],)]
        decidb.execute("select [1,2,3]")
        res_df = decidb.fetch_df()
        res = decidb.query("select * from res_df").fetchall()
        assert res == ref

    def test_duplicate(self):
        decidb.execute("create table tbl as select 5")
        dup_conn = decidb.duplicate()
        dup_conn.table("tbl").fetchall()
        decidb.execute("drop table tbl")
        with pytest.raises(decidb.CatalogException):
            dup_conn.table("tbl").fetchall()

    def test_readonly_properties(self):
        decidb.execute("select 42")
        description = decidb.description()
        rowcount = decidb.rowcount()
        assert description == [('42', 'NUMBER', None, None, None, None, None)]
        assert rowcount == -1

    def test_execute(self):
        assert [([4, 2],)] == decidb.execute("select [4,2]").fetchall()

    def test_executemany(self):
        # executemany does not keep an open result set
        # TODO: shouldn't we also have a version that executes a query multiple times with different parameters, returning all of the results?
        decidb.execute("create table tbl (i integer, j varchar)")
        decidb.executemany("insert into tbl VALUES (?, ?)", [(5, 'test'), (2, 'duck'), (42, 'quack')])
        res = decidb.table("tbl").fetchall()
        assert res == [(5, 'test'), (2, 'duck'), (42, 'quack')]
        decidb.execute("drop table tbl")

    def test_pystatement(self):
        with pytest.raises(decidb.ParserException, match='seledct'):
            statements = decidb.extract_statements('seledct 42; select 21')

        statements = decidb.extract_statements('select $1; select 21')
        assert len(statements) == 2
        assert statements[0].query == 'select $1'
        assert statements[0].type == decidb.StatementType.SELECT
        assert statements[0].named_parameters == set('1')
        assert statements[0].expected_result_type == [decidb.ExpectedResultType.QUERY_RESULT]

        assert statements[1].query == ' select 21'
        assert statements[1].type == decidb.StatementType.SELECT
        assert statements[1].named_parameters == set()

        with pytest.raises(
            decidb.InvalidInputException,
            match='Please provide either a DuckDBPyStatement or a string representing the query',
        ):
            rel = decidb.query(statements)

        with pytest.raises(decidb.BinderException, match="This type of statement can't be prepared!"):
            rel = decidb.query(statements[0])

        assert decidb.query(statements[1]).fetchall() == [(21,)]
        assert decidb.execute(statements[1]).fetchall() == [(21,)]

        with pytest.raises(
            decidb.InvalidInputException,
            match='Values were not provided for the following prepared statement parameters: 1',
        ):
            decidb.execute(statements[0])
        assert decidb.execute(statements[0], {'1': 42}).fetchall() == [(42,)]

        decidb.execute("create table tbl(a integer)")
        statements = decidb.extract_statements('insert into tbl select $1')
        assert statements[0].expected_result_type == [
            decidb.ExpectedResultType.CHANGED_ROWS,
            decidb.ExpectedResultType.QUERY_RESULT,
        ]
        with pytest.raises(
            decidb.InvalidInputException, match='executemany requires a non-empty list of parameter sets to be provided'
        ):
            decidb.executemany(statements[0])
        decidb.executemany(statements[0], [(21,), (22,), (23,)])
        assert decidb.table('tbl').fetchall() == [(21,), (22,), (23,)]
        decidb.execute("drop table tbl")

    def test_fetch_arrow_table(self):
        # Needed for 'fetch_arrow_table'
        pyarrow = pytest.importorskip("pyarrow")

        decidb.execute("Create Table test (a integer)")

        for i in range(1024):
            for j in range(2):
                decidb.execute("Insert Into test values ('" + str(i) + "')")
        decidb.execute("Insert Into test values ('5000')")
        decidb.execute("Insert Into test values ('6000')")
        sql = '''
        SELECT  a, COUNT(*) AS repetitions
        FROM    test
        GROUP BY a
        '''

        result_df = decidb.execute(sql).df()

        arrow_table = decidb.execute(sql).fetch_arrow_table()

        arrow_df = arrow_table.to_pandas()
        assert result_df['repetitions'].sum() == arrow_df['repetitions'].sum()
        decidb.execute("drop table test")

    def test_fetch_df(self):
        ref = [([1, 2, 3],)]
        decidb.execute("select [1,2,3]")
        res_df = decidb.fetch_df()
        res = decidb.query("select * from res_df").fetchall()
        assert res == ref

    def test_fetch_df_chunk(self):
        decidb.execute("CREATE table t as select range a from range(3000);")
        query = decidb.execute("SELECT a FROM t")
        cur_chunk = query.fetch_df_chunk()
        assert cur_chunk['a'][0] == 0
        assert len(cur_chunk) == 2048
        cur_chunk = query.fetch_df_chunk()
        assert cur_chunk['a'][0] == 2048
        assert len(cur_chunk) == 952
        decidb.execute("DROP TABLE t")

    def test_fetch_record_batch(self):
        # Needed for 'fetch_arrow_table'
        pyarrow = pytest.importorskip("pyarrow")

        decidb.execute("CREATE table t as select range a from range(3000);")
        decidb.execute("SELECT a FROM t")
        record_batch_reader = decidb.fetch_record_batch(1024)
        chunk = record_batch_reader.read_all()
        assert len(chunk) == 3000

    def test_fetchall(self):
        assert [([1, 2, 3],)] == decidb.execute("select [1,2,3]").fetchall()

    def test_fetchdf(self):
        ref = [([1, 2, 3],)]
        decidb.execute("select [1,2,3]")
        res_df = decidb.fetchdf()
        res = decidb.query("select * from res_df").fetchall()
        assert res == ref

    def test_fetchmany(self):
        assert [(0,), (1,)] == decidb.execute("select * from range(5)").fetchmany(2)

    def test_fetchnumpy(self):
        numpy = pytest.importorskip("numpy")
        decidb.execute("SELECT BLOB 'hello'")
        results = decidb.fetchall()
        assert results[0][0] == b'hello'

        decidb.execute("SELECT BLOB 'hello' AS a")
        results = decidb.fetchnumpy()
        assert results['a'] == numpy.array([b'hello'], dtype=object)

    def test_fetchone(self):
        assert (0,) == decidb.execute("select * from range(5)").fetchone()

    def test_from_arrow(self):
        assert None != decidb.from_arrow

    def test_from_csv_auto(self):
        assert None != decidb.from_csv_auto

    def test_from_df(self):
        assert None != decidb.from_df

    def test_from_parquet(self):
        assert None != decidb.from_parquet

    def test_from_query(self):
        assert None != decidb.from_query

    def test_get_table_names(self):
        assert None != decidb.get_table_names

    def test_install_extension(self):
        assert None != decidb.install_extension

    def test_load_extension(self):
        assert None != decidb.load_extension

    def test_query(self):
        assert [(3,)] == decidb.query("select 3").fetchall()

    def test_register(self):
        assert None != decidb.register

    def test_register_relation(self):
        con = decidb.connect()
        rel = con.sql('select [5,4,3]')
        con.register("relation", rel)

        con.sql("create table tbl as select * from relation")
        assert con.table('tbl').fetchall() == [([5, 4, 3],)]

    def test_unregister_problematic_behavior(self, duckdb_cursor):
        # We have a VIEW called 'vw' in the Catalog
        duckdb_cursor.execute("create temporary view vw as from range(100)")
        assert duckdb_cursor.execute("select * from vw").fetchone() == (0,)

        # Create a registered object called 'vw'
        arrow_result = duckdb_cursor.execute("select 42").arrow()
        with pytest.raises(decidb.CatalogException, match='View with name "vw" already exists'):
            duckdb_cursor.register('vw', arrow_result)

        # Temporary views take precedence over registered objects
        assert duckdb_cursor.execute("select * from vw").fetchone() == (0,)

        # Decide that we're done with this registered object..
        duckdb_cursor.unregister('vw')

        # This should not have affected the existing view:
        assert duckdb_cursor.execute("select * from vw").fetchone() == (0,)

    @pytest.mark.parametrize('pandas', [NumpyPandas(), ArrowPandas()])
    def test_relation_out_of_scope(self, pandas):
        def temporary_scope():
            # Create a connection, we will return this
            con = decidb.connect()
            # Create a dataframe
            df = pandas.DataFrame({'a': [1, 2, 3]})
            # The dataframe has to be registered as well
            # making sure it does not go out of scope
            con.register("df", df)
            rel = con.sql('select * from df')
            con.register("relation", rel)
            return con

        con = temporary_scope()
        res = con.sql('select * from relation').fetchall()
        print(res)

    def test_table(self):
        con = decidb.connect()
        con.execute("create table tbl as select 1")
        assert [(1,)] == con.table("tbl").fetchall()

    def test_table_function(self):
        assert None != decidb.table_function

    def test_unregister(self):
        assert None != decidb.unregister

    def test_values(self):
        assert None != decidb.values

    def test_view(self):
        decidb.execute("create view vw as select range(5)")
        assert [([0, 1, 2, 3, 4],)] == decidb.view("vw").fetchall()
        decidb.execute("drop view vw")

    def test_description(self):
        assert None != decidb.description

    def test_close(self):
        assert None != decidb.close

    def test_interrupt(self):
        assert None != decidb.interrupt

    def test_wrap_shadowing(self):
        pd = NumpyPandas()
        import decidb

        df = pd.DataFrame({"a": [1, 2, 3]})
        res = decidb.sql("from df").fetchall()
        assert res == [(1,), (2,), (3,)]

    def test_wrap_coverage(self):
        con = decidb.default_connection

        # Skip all of the initial __xxxx__ methods
        connection_methods = dir(con)
        filtered_methods = [method for method in connection_methods if not is_dunder_method(method)]
        for method in filtered_methods:
            # Assert that every method of DuckDBPyConnection is wrapped by the 'decidb' module
            assert method in dir(decidb)

    def test_connect_with_path(self, tmp_database):
        import pathlib

        assert isinstance(tmp_database, pathlib.Path)
        con = decidb.connect(tmp_database)
        assert con.sql("select 42").fetchall() == [(42,)]

        with pytest.raises(
            decidb.InvalidInputException, match="Please provide either a str or a pathlib.Path, not <class 'int'>"
        ):
            con = decidb.connect(5)

    def test_set_pandas_analyze_sample_size(self):
        con = decidb.connect(":memory:named", config={"pandas_analyze_sample": 0})
        res = con.sql("select current_setting('pandas_analyze_sample')").fetchone()
        assert res == (0,)

        # Find the cached config
        con2 = decidb.connect(":memory:named", config={"pandas_analyze_sample": 0})
        con2.execute(f"SET GLOBAL pandas_analyze_sample=2")

        # This change is reflected in 'con' because the instance was cached
        res = con.sql("select current_setting('pandas_analyze_sample')").fetchone()
        assert res == (2,)
