import decidb


class TestMap(object):

    def test_array_list_tuple_ambiguity(self):
        con = decidb.connect()
        res = con.sql("SELECT $arg", params={'arg': (1, 2)}).fetchall()[0][0]
        assert res == [1, 2]

        # By using an explicit decidb.Value with an array type, we should convert the input as an array
        # and get an array (tuple) back
        typ = decidb.array_type(decidb.typing.BIGINT, 2)
        val = decidb.Value((1, 2), typ)
        res = con.sql("SELECT $arg", params={'arg': val}).fetchall()[0][0]
        assert res == (1, 2)

        val = decidb.Value([3, 4], typ)
        res = con.sql("SELECT $arg", params={'arg': val}).fetchall()[0][0]
        assert res == (3, 4)
