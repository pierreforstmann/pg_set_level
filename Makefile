MODULES = pg_set_level 
EXTENSION = pg_set_level  # the extension's name
DATA = pg_set_level--0.0.1.sql    # script file to install
#REGRESS = xxx      # the test script file

# for posgres build
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

pgxn:
	git archive --format zip  --output ../pgxn/pg_set_level/pg_set_level-0.0.1.zip master
