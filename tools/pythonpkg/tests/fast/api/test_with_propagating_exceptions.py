import pytest
import decidb


class TestWithPropagatingExceptions(object):
    def test_with(self):
        # Should propagate exception raised in the 'with decidb.connect() ..'
        with pytest.raises(decidb.ParserException, match="syntax error at or near *"):
            with decidb.connect() as con:
                print('before')
                con.execute('invalid')
                print('after')

        # Does not raise an exception
        with decidb.connect() as con:
            print('before')
            con.execute('select 1')
            print('after')
