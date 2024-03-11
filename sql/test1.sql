--
-- test1.sql
--
create extension pg_set_level;
--
show work_mem;
set work_mem='1GB';
show work_mem;
--
show search_path;
set search_path='';
show search_path;
--
show datestyle;
set datestyle='dmy';
show datestyle;
--
show log_statement;
set log_statement='all';
show log_statement;
--
show log_min_messages;
set log_min_messages=debug1;
-- output only in instance log
show log_min_messages;
--
-- not possible to test FATAL because breaks connection with PG
