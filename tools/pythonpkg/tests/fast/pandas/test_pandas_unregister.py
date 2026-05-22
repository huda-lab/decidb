import decidb
import pytest
import tempfile
import os
import gc
import pytest
from conftest import NumpyPandas, ArrowPandas


class TestPandasUnregister(object):
    @pytest.mark.parametrize('pandas', [NumpyPandas(), ArrowPandas()])
    def test_pandas_unregister1(self, duckdb_cursor, pandas):
        df = pandas.DataFrame([[1, 2, 3], [4, 5, 6]])
        connection = decidb.connect(":memory:")
        connection.register("dataframe", df)

        df2 = connection.execute("SELECT * FROM dataframe;").fetchdf()
        connection.unregister("dataframe")
        with pytest.raises(decidb.CatalogException, match='Table with name dataframe does not exist'):
            connection.execute("SELECT * FROM dataframe;").fetchdf()
        with pytest.raises(decidb.CatalogException, match='View with name dataframe does not exist'):
            connection.execute("DROP VIEW dataframe;")
        connection.execute("DROP VIEW IF EXISTS dataframe;")

    @pytest.mark.parametrize('pandas', [NumpyPandas(), ArrowPandas()])
    def test_pandas_unregister2(self, duckdb_cursor, pandas):
        fd, db = tempfile.mkstemp()
        os.close(fd)
        os.remove(db)

        connection = decidb.connect(db)
        df = pandas.DataFrame([[1, 2, 3], [4, 5, 6]])

        connection.register("dataframe", df)
        connection.unregister("dataframe")  # Attempting to unregister.
        connection.close()

        # Reconnecting while DataFrame still in mem.
        connection = decidb.connect(db)
        assert len(connection.execute("PRAGMA show_tables;").fetchall()) == 0

        with pytest.raises(decidb.CatalogException, match='Table with name dataframe does not exist'):
            connection.execute("SELECT * FROM dataframe;").fetchdf()

        connection.close()

        del df
        gc.collect()

        # Reconnecting after DataFrame freed.
        connection = decidb.connect(db)
        assert len(connection.execute("PRAGMA show_tables;").fetchall()) == 0
        with pytest.raises(decidb.CatalogException, match='Table with name dataframe does not exist'):
            connection.execute("SELECT * FROM dataframe;").fetchdf()
        connection.close()
