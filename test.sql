drop table if exists foo, bar, baz cascade;

create table foo (a int default 42, b timestamp, c text);
insert into foo values (142857, now(), 'hello world');
insert into foo values (default, now() - interval '1 day', 'goodbye');

begin;
update pg_attribute set attlognum = 1 where attname = 'c' and attrelid = 'foo'::regclass;
update pg_attribute set attlognum = 2 where attname = 'a' and attrelid = 'foo'::regclass;
update pg_attribute set attlognum = 3 where attname = 'b' and attrelid = 'foo'::regclass;
commit;

select * from foo;
select foo from foo;
select foo.* from foo;

create function f() returns setof foo language sql as $$
  select * from foo;
$$;
select * from f();

select row('oh', 1125, 'today')::foo;
select (row('oh', 1125, 'today')::foo).*;
insert into foo select (row('oh', 1125, 'today')::foo).*;
insert into foo values ('values one', 1, '2008-10-20'), ('values two', 2, '2004-08-15');
copy foo from stdin;
copy one	1001	1998-12-10 23:54
copy two	1002	1996-08-01 09:22
\.
select * from foo order by 2;

create table bar (x text, y int default 142857, z timestamp );
insert into bar values ('oh no', default, now() - interval '1 month');
insert into bar values ('oh yes', 42, now() - interval '1 year');
begin;
update pg_attribute set attlognum = 3 where attname = 'x' and attrelid = 'bar'::regclass;
update pg_attribute set attlognum = 1 where attname = 'z' and attrelid = 'bar'::regclass;
commit;


select foo.* from bar, foo where bar.y = foo.a;
select bar.* from bar, foo where bar.y = foo.a;
select * from bar, foo where bar.y = foo.a;
select * from foo join bar on (foo.a = bar.y);

alter table bar rename y to a;
select * from foo natural join bar;
select * from foo join bar using (a);

create table baz (e point) inherits (foo, bar);
create table baz (e point, a int default 23) inherits (foo, bar);
insert into baz (e) values ('(1,1)');
select * from foo;
select * from bar;
select * from baz;



create table quux (a int, b int[], c int);
begin;
update pg_attribute set attlognum = 1 where attnum = 2 and attrelid = 'quux'::regclass;
update pg_attribute set attlognum = 2 where attnum = 1 and attrelid = 'quux'::regclass;
commit;
select * from quux where (a,c) in ( select a,c from quux );

