from .. import USE_ACTUAL_SPARK

if USE_ACTUAL_SPARK:
    from pyspark.sql.functions import *
else:
    from decidb.experimental.spark.sql.functions import *
