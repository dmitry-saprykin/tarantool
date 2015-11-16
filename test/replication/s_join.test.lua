env = require('test_run')
test_run = env.new()
box.schema.user.grant('guest', 'read,write,execute', 'universe')
box.schema.user.grant('guest', 'replication')
space = box.schema.space.create('test', { id = 99999, engine = "sophia" })
index = space:create_index('primary', { type = 'tree'})
for k = 1, 123 do space:insert{k, k*k} end
box.snapshot()

-- replica join
test_run:cmd("create server replica with rpl_master=default, script='replication/replica.lua'")
test_run:cmd("start server replica")
test_run:cmd('wait_lsn replica default')

test_run:cmd('switch replica')
box.space.test:select()

test_run:cmd('switch default')
test_run:cmd("stop server replica")
test_run:cmd("cleanup server replica")
space:drop()
box.snapshot()
box.schema.user.revoke('guest', 'replication')
