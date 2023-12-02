local t = require('luatest')
local cbuilder = require('test.config-luatest.cbuilder')
local replicaset = require('test.config-luatest.replicaset')

local g = t.group()

g.before_all(replicaset.init)
g.after_each(replicaset.drop)
g.after_all(replicaset.clean)

-- Verify that an anonymous replica can be started and joined to
-- a replicaset.
--
-- This test excludes the `replication.peers` autoconstruction
-- logic by defining the option on its own. This automatic
-- construction is verified in a separate test case (TODO).
g.test_basic = function(g)
    -- One master, two replicas, two anonymous replicas.
    local config = cbuilder.new()
        :set_replicaset_option('replication.peers', {
            'replicator:secret@unix/:./instance-001.iproto',
            'replicator:secret@unix/:./instance-002.iproto',
            'replicator:secret@unix/:./instance-003.iproto',
        })
        :add_instance('instance-001', {
            database = {
                mode = 'rw',
            },
        })
        :add_instance('instance-002', {})
        :add_instance('instance-003', {})
        :add_instance('instance-004', {
            replication = {
                anon = true,
            },
        })
        :add_instance('instance-005', {
            replication = {
                anon = true,
            },
        })
        :config()

    local replicaset = replicaset.new(g, config)
    replicaset:start()

    -- Verify that all the instances are healthy.
    replicaset:each(function(server)
        server:exec(function()
            t.assert_equals(box.info.status, 'running')
        end)
    end)

    -- Verify that the anonymous replicas are actually anonymous.
    replicaset['instance-004']:exec(function()
        t.assert_equals(box.info.id, 0)
    end)
    replicaset['instance-005']:exec(function()
        t.assert_equals(box.info.id, 0)
    end)
end
