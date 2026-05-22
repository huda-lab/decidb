import decidb


class TestModule:
    def test_paramstyle(self):
        assert decidb.paramstyle == "qmark"

    def test_threadsafety(self):
        assert decidb.threadsafety == 1

    def test_apilevel(self):
        assert decidb.apilevel == "2.0"
