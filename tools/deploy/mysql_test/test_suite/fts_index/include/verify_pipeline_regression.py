#!/usr/bin/env python3
"""Compare repeated TOKENIZE calls with default-property uncached calls and DDL semantics."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import pymysql

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',type=int,default=2881)
p.add_argument('--output',required=True)
p.add_argument('--revision')
a=p.parse_args()

def connect():
    return pymysql.connect(host='127.0.0.1',port=a.port,user='root',charset='utf8mb4',autocommit=True,read_timeout=180)

texts=[None,'','database database audit','OceanBase 是分布式数据库，全文索引检索。','café Straße 日本語 😀',
       "quotes ' \\ zero\x00 end",'A'*50000] + ['term'+str(i)+' 数据库 foo bar' for i in range(12)]

def compare(worker):
    checks=0
    with connect() as conn,conn.cursor() as c:
        for repeat in range(3):
            for parser in ('ik','beng',' IK ','BENG'):
                for text in texts[worker%3:]+texts[:worker%3]:
                    c.execute('SELECT TOKENIZE(CAST(%s AS CHAR CHARACTER SET utf8mb4),%s),TOKENIZE(CAST(%s AS CHAR CHARACTER SET utf8mb4),%s,JSON_ARRAY())',(text,parser,text,parser))
                    left,right=c.fetchone()
                    assert json.loads(left)==json.loads(right),(worker,repeat,parser,text)
                    checks+=1
        # Changing values on the same prepared expression cannot reuse an old key.
        c.execute("PREPARE memo_stmt FROM 'SELECT TOKENIZE(CAST(? AS CHAR CHARACTER SET utf8mb4),?),TOKENIZE(CAST(? AS CHAR CHARACTER SET utf8mb4),?,JSON_ARRAY())'")
        for text in texts[:6]:
            c.execute("SET @t=%s,@p='beng'",(text,))
            c.execute('EXECUTE memo_stmt USING @t,@p,@t,@p')
            left,right=c.fetchone()
            assert json.loads(left)==json.loads(right)
            checks+=1
        c.execute('DEALLOCATE PREPARE memo_stmt')
        for collation in ('utf8mb4_bin','utf8mb4_general_ci'):
            c.execute(f"SELECT TOKENIZE(_utf8mb4'ABC abc 数据库' COLLATE {collation},'beng'),TOKENIZE(_utf8mb4'ABC abc 数据库' COLLATE {collation},'beng',JSON_ARRAY())")
            left,right=c.fetchone();assert json.loads(left)==json.loads(right);checks+=1
        for parser in ('invalid_parser','',None):
            outcomes=[]
            for props in ('',',JSON_ARRAY()'):
                try:
                    c.execute('SELECT TOKENIZE(%s,%s'+props+')',('test',parser))
                    outcomes.append(('value',c.fetchone()[0]))
                except pymysql.MySQLError as e:
                    outcomes.append(('error',e.args[0]))
            assert outcomes[0]==outcomes[1],outcomes
            checks+=1
    return checks

with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    checks=sum(pool.map(compare,range(6)))
with connect() as conn,conn.cursor() as c:
    c.execute('DROP DATABASE IF EXISTS fts_pipeline_ddl')
    c.execute('CREATE DATABASE fts_pipeline_ddl')
    c.execute('USE fts_pipeline_ddl')
    c.execute('CREATE TABLE t(id INT PRIMARY KEY,k INT,v VARCHAR(100),g INT GENERATED ALWAYS AS (k+7) STORED)')
    c.executemany('INSERT INTO t(id,k,v) VALUES(%s,%s,%s)',[(i,i%137,'value '+str(i)) for i in range(3000)])
    c.execute('ALTER TABLE t ADD INDEX idx(k,g)')
    c.execute('ALTER TABLE t ADD UNIQUE INDEX uniq(v)')
    for key in (0,1,36,136):
        c.execute('SELECT id,g FROM t FORCE INDEX(idx) WHERE k=%s ORDER BY id',(key,));indexed=c.fetchall()
        c.execute('SELECT id,g FROM t IGNORE INDEX(idx,uniq) WHERE k=%s ORDER BY id',(key,));assert c.fetchall()==indexed
    try:
        c.execute('ALTER TABLE t ADD UNIQUE INDEX invalid_uniq(k)')
    except pymysql.MySQLError as e:
        assert e.args[0]==1062,e.args
    else:
        raise AssertionError('Duplicate unique index accepted')
    c.execute('SELECT COUNT(*),SUM(id) FROM t');assert c.fetchone()==(3000,4498500)
    c.execute('DROP DATABASE fts_pipeline_ddl')
report={'revision':a.revision,'passed':True,'tokenize_comparisons':checks,'ddl_rows':3000,'concurrent_connections':6}
Path(a.output).write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(report))
