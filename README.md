# pg_set_level
pg_set_level is a PostgreSQL extension which allows to customize the SET statement.

# Installation
## Compiling

This module can be built using the standard PGXS infrastructure. For this to work, the pg_config program must be available in your $PATH:
  
`git clone https://github.com/pierreforstmann/pg_set_level.git` <br>
`cd pg_set_level` <br>
`make` <br>
`make install` <br>

## PostgreSQL setup

Extension must be loaded at server level with `shared_preload_libraries` parameter: <br> 
`shared_preload_libraries = 'pg_set_level'` <br>

It can also be created with following SQL statement at server level:<br>
`create extension pg_set_level;` <br>

This extension has been validated with PostgreSQL 9.5, 9.6, 10, 11, 12 and 13.

## Usage
pg_set_level has 2 specific GUC parameters:<br>
- `pg_set_level.names` which the list of GUC comma separated parameters for which SET statement must be modified <br>
- `ps_set_level.action` which is the action to run when SET statement is run. Action can be one the message severity level: INFO, NOTICE, WARNING, ERROR, LOG, FATAL. <br>
The same action is run for each parameter.

Note that pg_set_level only works for the SET statement: it does take into account `set_config` function call for any GUC parameter. <br>

## Example
To generate an error when `SET work_mem` is run, add in postgresql.conf: <br>
`pg_set_level.names='work_mem'` <br>
`pg_set_level.action='error'`  <br>

Run from psql:<br>
`# show work_mem;i`<br>
` work_mem`<br>
`----------` <br>
` 4MB `<br>
`(1 row)`

`# set work_mem='1G';` <br>
`ERROR:  pg_set_level: set work_mem='1G';` <br>


