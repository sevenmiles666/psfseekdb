# MiniVectorDB

基于官方 `2026-competition-round2` 提交 `c842a368c3394748d0499386c18f6b465242e141` 开发的轻量向量数据库。当前版本提供 C++17 引擎、Linux x86_64 容器和本地测试接口。

## 构建和运行

Dockerfile 锁定 GCC 13.4.0 构建镜像的摘要，从源码编译静态可执行文件。引擎使用 C++ 标准库和 Linux 文件接口。容器以前台方式处理标准输入，数据目录为 `/data`。

```bash
docker build --platform linux/amd64 -t minivectordb-round2:dev .
docker run --rm -i --network=none -v minivectordb-demo:/data minivectordb-round2:dev
```

使用空数据卷时，可逐行输入下面的 SQL。每条语句一行，末尾分号可省略。

```sql
CREATE TABLE items (id BIGINT PRIMARY KEY, label VARCHAR(12), v VECTOR(2));
INSERT INTO items VALUES (1,'near',[1,0]),(2,'near',[2,0]),(3,'keep',[3,0]),(4,'keep',[4,0]);
SELECT * FROM items WHERE id>=2 AND id<=3 ORDER BY id;
SELECT id,L2_DISTANCE(v,[0,0]) AS distance FROM items WHERE label='keep' ORDER BY distance LIMIT 2;
BEGIN;
UPDATE items SET v=[0,0] WHERE id=3;
SELECT id,L2_DISTANCE(v,[0,0]) FROM items WHERE id=3;
ROLLBACK;
BEGIN;
UPDATE items SET label='saved' WHERE id=4;
COMMIT;
```

输入 `\q` 或关闭标准输入结束进程。用同一个数据卷重开后，`SELECT id,label FROM items ORDER BY id;` 仍会返回 `id=4` 的 `saved` 标签。第一个事务中对 `id=3` 的向量修改已被回滚。

本地接口启动时输出 `READY<TAB>minivectordb-local-v1`，查询逐行输出 `ROW<TAB><JSON 数组>`，结束输出 `OK<TAB><行数>`。失败输出 `ERR<TAB><错误码><TAB><消息>`。每条响应结束后刷新 stdout，进程级 I/O 错误写入 stderr。最终评测帧格式和错误码由官方题包确定，适配入口集中在 `src/main.cpp`，引擎入口为 `Database::execute`。

## 实现范围

| 模块 | 行为 |
| --- | --- |
| 表和修改 | `CREATE TABLE`、多行 `INSERT`、条件 `UPDATE` 和 `DELETE`。每表一个 BIGINT 主键，支持 VARCHAR 和一个 VECTOR 列。 |
| 查询 | 普通投影、`SELECT *`、`AND`、`= < <= > >=`、`ORDER BY`、`LIMIT`。排序键相同的记录按主键升序输出。 |
| 精确 TopK | 先应用 `WHERE`，再按存储后的 float32 向量计算距离。用 double 累加平方差，输出欧氏距离。 |
| 输入检查 | 检查主键重复、BIGINT 范围、UTF-8 字符数、向量维度及有限 float32。未支持的 SQL 返回错误。 |
| 事务 | 显式 `BEGIN`、`COMMIT`、`ROLLBACK` 和隐式自动提交。事务内查询使用当前可见行。一条修改失败时撤销该语句，保留事务内此前成功的语句。 |
| 恢复 | 使用提交标记确定持久化边界，重放已提交内容并清理未提交或不完整的尾部后发出就绪标识。 |

`store.wal` 保存表定义和行修改。每条记录有长度和 CRC32，提交成功响应在写入提交标记并完成 `fdatasync` 之后发出。内存中的主键表记录每行的文件偏移，行数据按需解码。语句和事务回滚恢复偏移并截断相应日志尾部。

精确 TopK 使用有限候选堆。较大的排序按约 2 MiB 的键预算生成有序段，最多八路合并，临时文件也位于数据目录。主键表和未提交事务的撤销记录仍占内存，超长单行和单条 SQL 请求也需要内存。WAL 按历史追加，启动会扫描日志；历史压缩待后续实现。

本阶段交付 exact 路径。`CREATE VECTOR INDEX`、ANN 和正式协议适配待配套题包确认后接入。学习材料暂按赛题说明的默认位置存放在 `agent/conversation.json`，内容区分用户原话、Agent 操作和测试结果。

## 本地验证

```bash
python tests/check_core.py --image minivectordb-round2:dev --large
```

在 Linux 或 Ubuntu WSL 中也可直接编译核心并运行测试：

```bash
mkdir -p build
g++ -std=c++17 -O2 -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Wpedantic -Werror \
  src/main.cpp src/sql.cpp src/journal.cpp src/database.cpp -o build/minivectordb
python3 tests/check_core.py --binary build/minivectordb
```

2026-11-09，Linux 本地构建和断网容器测试均通过。用独立计算结果核对了 24 组 128 维精确 TopK，其中 12 组带过滤条件。提交后强杀、未提交尾部、语句级撤销、EOF 回滚及断帧重放均通过。32 MiB 内存限制的容器成功重开、查询和排序 52,482,580 字节的持久化数据。正式 MySQL case 和两榜成绩由比赛评测产生。
