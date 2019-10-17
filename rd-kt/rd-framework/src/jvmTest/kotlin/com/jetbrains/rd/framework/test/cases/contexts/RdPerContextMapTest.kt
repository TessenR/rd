@file:Suppress("UNCHECKED_CAST")
package com.jetbrains.rd.framework.test.cases.contexts

import com.jetbrains.rd.framework.FrameworkMarshallers
import com.jetbrains.rd.framework.ISerializer
import com.jetbrains.rd.framework.RdContextKey
import com.jetbrains.rd.framework.base.static
import com.jetbrains.rd.framework.impl.RdMap
import com.jetbrains.rd.framework.impl.RdPerContextMap
import com.jetbrains.rd.framework.test.util.DynamicEntity
import com.jetbrains.rd.framework.test.util.RdFrameworkTestBase
import com.jetbrains.rd.util.assert
import com.jetbrains.rd.util.lifetime.onTermination
import org.junit.Assert
import org.junit.BeforeClass
import org.junit.Test

class RdPerContextMapTest : RdFrameworkTestBase() {
    companion object {
        @BeforeClass
        @JvmStatic
        fun resetContext() {
            RdContextKey<String>("test-key", true, FrameworkMarshallers.String).value = null
        }
    }

    @Test
    fun testOnStructMap() {
        val key = RdContextKey<String>("test-key", true, FrameworkMarshallers.String)

        val serverMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, FrameworkMarshallers.String).apply { master = it } }.static(1)
        val clientMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, FrameworkMarshallers.String).apply { master = it } }.static(1).apply { master = false }

        val server1Cid = "Server-1"
        val client1Cid = "Client-1"

        serverProtocol.contextHandler.registerKey(key)
        clientProtocol.contextHandler.registerKey(key)

        serverProtocol.contextHandler.getValueSet(key).add(server1Cid)

        clientMap.bind(clientLifetime, clientProtocol, "map")
        serverMap.bind(serverLifetime, serverProtocol, "map")
        serverMap[server1Cid]!![1] = "test"

        assert(clientMap[server1Cid]!![1] == "test")

        clientProtocol.contextHandler.getValueSet(key).add(client1Cid)

        assert(serverMap[client1Cid] != null)
        assert(serverMap[client1Cid]!![1] == null)
    }

    @Test
    fun testOnDynamicMap() {
        val key = RdContextKey<String>("test-key", true, FrameworkMarshallers.String)

        val serverMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1)
        val clientMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1).apply { master = false }

        val server1Cid = "Server-1"
        val client1Cid = "Client-1"

        serverProtocol.contextHandler.registerKey(key)
        clientProtocol.contextHandler.registerKey(key)

        serverProtocol.contextHandler.getValueSet(key).add(server1Cid)

        clientMap.bind(clientLifetime, clientProtocol, "map")
        serverMap.bind(serverLifetime, serverProtocol, "map")
        serverMap[server1Cid]!![1] = DynamicEntity("test")

        assert(clientMap[server1Cid]!![1]!!.foo.value == "test")

        clientProtocol.contextHandler.getValueSet(key).add(client1Cid)

        assert(serverMap[client1Cid] != null)
        assert(serverMap[client1Cid]!![1] == null)
    }

    @Test
    fun testLateBind01() {
        val key = RdContextKey<String>("test-key", true, FrameworkMarshallers.String)

        val serverMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1)
        val clientMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1).apply { master = false }

        val server1Cid = "Server-1"
        val client1Cid = "Client-1"

        serverProtocol.contextHandler.registerKey(key)
        clientProtocol.contextHandler.registerKey(key)

        serverProtocol.contextHandler.getValueSet(key).add(server1Cid)

        serverMap[server1Cid]!![1] = DynamicEntity("test")
        clientProtocol.contextHandler.getValueSet(key).add(client1Cid)

        clientMap.bind(clientLifetime, clientProtocol, "map")
        serverMap.bind(serverLifetime, serverProtocol, "map")

        assert(clientMap[server1Cid]!![1]!!.foo.value == "test")

        assert(serverMap[client1Cid] != null)
        assert(serverMap[client1Cid]!![1] == null)
    }

    @Test
    fun testLateBind02() {
        val key = RdContextKey<String>("test-key", true, FrameworkMarshallers.String)

        val serverMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1)
        val clientMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1).apply { master = false }

        val server1Cid = "Server-1"

        serverProtocol.contextHandler.registerKey(key)
        clientProtocol.contextHandler.registerKey(key)

        // no protocol value set value - pre-bind value will be lost

        serverMap[server1Cid]!![1] = DynamicEntity("test")

        clientMap.bind(clientLifetime, clientProtocol, "map")
        serverMap.bind(serverLifetime, serverProtocol, "map")

        assert(serverMap[server1Cid] == null)
        assert(clientMap[server1Cid] == null)
    }

    @Test
    fun testLateBind03() {
        val key = RdContextKey<String>("test-key", true, FrameworkMarshallers.String)

        val serverMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1)
        val clientMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1).apply { master = false }

        val server1Cid = "Server-1"
        val server2Cid = "Server-2"

        serverProtocol.contextHandler.registerKey(key)
        clientProtocol.contextHandler.registerKey(key)

        serverProtocol.contextHandler.getValueSet(key).addAll(setOf(server1Cid, server2Cid))

        val log = ArrayList<String>()

        serverMap.view(serverLifetime) { entryLt, k, _ ->
            log.add("Add $k")
            entryLt.onTermination {
                log.add("Remove $k")
            }
        }

        serverMap[server1Cid]!![1] = DynamicEntity("test")
        serverMap[server2Cid]!![1] = DynamicEntity("test")

        clientMap.bind(clientLifetime, clientProtocol, "map")
        serverMap.bind(serverLifetime, serverProtocol, "map")

        Assert.assertEquals(listOf("Add $server1Cid", "Add $server2Cid"), log)
    }

    @Test
    fun testLateBind04() {
        val key = RdContextKey<String>("test-key", true, FrameworkMarshallers.String)

        val serverMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1)
        val clientMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1).apply { master = false }

        val server1Cid = "Server-1"
        val server2Cid = "Server-2"

        serverProtocol.contextHandler.registerKey(key)
        clientProtocol.contextHandler.registerKey(key)

        serverProtocol.contextHandler.getValueSet(key).addAll(setOf(server1Cid))

        val log = ArrayList<String>()

        serverMap.view(serverLifetime) { entryLt, k, _ ->
            log.add("Add $k")
            entryLt.onTermination {
                log.add("Remove $k")
            }
        }

        serverMap[server1Cid]!![1] = DynamicEntity("test")
        serverMap[server2Cid]!![1] = DynamicEntity("test")

        clientMap.bind(clientLifetime, clientProtocol, "map")
        serverMap.bind(serverLifetime, serverProtocol, "map")

        Assert.assertEquals(listOf("Add $server1Cid", "Add $server2Cid", "Remove $server2Cid"), log)
    }

    @Test
    fun testLateBind05() {
        val key = RdContextKey<String>("test-key", true, FrameworkMarshallers.String)

        val serverMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1)
        val clientMap = RdPerContextMap(key) { RdMap(FrameworkMarshallers.Int, DynamicEntity as ISerializer<DynamicEntity<String>>).apply { master = it } }.static(1).apply { master = false }

        val server1Cid = "Server-1"

        serverProtocol.contextHandler.registerKey(key)
        clientProtocol.contextHandler.registerKey(key)

        val log = ArrayList<String>()

        serverMap.view(serverLifetime) { entryLt, k, _ ->
            log.add("Add $k")
            entryLt.onTermination {
                log.add("Remove $k")
            }
        }

        serverProtocol.contextHandler.getValueSet(key).addAll(setOf(server1Cid))

        clientMap.bind(clientLifetime, clientProtocol, "map")
        serverMap.bind(serverLifetime, serverProtocol, "map")

        serverMap[server1Cid]!![1] = DynamicEntity("test")

        Assert.assertEquals(listOf("Add $server1Cid"), log)
    }
}
