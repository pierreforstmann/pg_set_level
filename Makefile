#
# Makefile
# 
MODULES = pg_set_level 
EXTENSION = pg_set_level  # the extension's name
DATA = pg_set_level--0.0.1.sql    # script file to install


# run: make clean + make + make install + make installcheck
REGRESS_OPTS =  --temp-instance=/tmp/5555 --port=5555 --temp-config pgsl.conf
REGRESS = test1 

# for posgres build
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

pgxn:
	git archive --format zip  --output ../pgxn/pg_set_level/pg_set_level-0.0.1.zip master
