_G.Dialogue = require("Dialogue")

describe("An Actor", function()
    local actor = actor

    it("is created by passing it a table of the form {'module' [, arg1, arg2, ..., argn]}", function()
        actor = Dialogue.Actor.new{ {"draw", 400, 600} }
        assert.is_equal(1, #actor:scripts())
    end)

    it("is loaded almost immediately in its own thread", function()
        os.execute("sleep " .. tonumber(0.5))
        local script = actor:scripts()[1]
        assert.are.same({400, 600}, script:probe("coordinates"))
    end)

    it("can handle being garbage collected", function()
        actor = nil
        collectgarbage()
        actor = Dialogue.Actor.new{ {"draw", 1, 2}, {"draw", 3, 4} }
        os.execute("sleep " .. tonumber(0.5))
        assert.is_equal(2, #actor:scripts())
        assert.are.same({1, 2}, actor:scripts()[1]:probe("coordinates"))
        assert.are.same({3, 4}, actor:scripts()[2]:probe("coordinates"))
    end)

    it("can handle Scripts erroring out gracefully", function()
        local errfn = function()
            actor = Dialogue.Actor.new{ {"bad", 2, 4} }
            os.execute("sleep " .. tonumber(0.5))
            actor:scripts()[1]:probe("coordinates")
        end
        assert.has_error(errfn, "Cannot Probe: The Script's module isn't valid or has errors.")
    end)

    it("can be sent messages of the form {'method' [, arg1, arg2, ..., argn]}", function()
        actor = Dialogue.Actor.new{ {"draw", 1, 2} }
        os.execute("sleep " .. tonumber(0.5))
        actor:send{"move", 1, 1}
        os.execute("sleep " .. tonumber(0.5))
        assert.are.same({2, 3}, actor:scripts()[1]:probe("coordinates"))
    end)

    it("skips sending messages to Scripts which have errors", function()
        actor = Dialogue.Actor.new{ {"bad"}, {"draw", 250, 250} }
        os.execute("sleep " .. tonumber(0.5))
        actor:send{"move", 1, 1}
        os.execute("sleep " .. tonumber(0.5))
        assert.are.same({251, 251}, actor:scripts()[2]:probe("coordinates"))
    end)

    it("can give a bad script a new module and definition to load", function()
        actor:scripts()[1]:load{"draw", 5, 5}
        os.execute("sleep " .. tonumber(0.5))
        actor:send{"move", 1, 1}
        os.execute("sleep " .. tonumber(0.5))
        assert.are.same({6, 6}, actor:scripts()[1]:probe("coordinates"))
        assert.are.same({252, 252}, actor:scripts()[2]:probe("coordinates"))
    end)

    it("can handle manually reloading scripts to its original state for any reason", function()
        actor:scripts()[2]:load()
        os.execute("sleep " .. tonumber(0.5))
        assert.are.same({250, 250}, actor:scripts()[2]:probe("coordinates"))
    end)

    it("can handle removing any script", function()
        assert.is_equal(2, #actor:scripts())
        actor:scripts()[1]:remove()
        os.execute("sleep " .. tonumber(0.5))
        assert.is_equal(1, #actor:scripts())
        assert.are.same({250, 250}, actor:scripts()[1]:probe("coordinates"))
    end)

    it("can reload all of its scripts", function()
        actor:load()
        os.execute("sleep " .. tonumber(0.5))
        assert.is_equal(1, #actor:scripts())
        actor:send{"move", -250, -250}
        os.execute("sleep " .. tonumber(0.5))
        assert.are.same({0, 0}, actor:scripts()[1]:probe("coordinates"))
    end)

    it("has special 'Lead Actor-only' methods", function()
        local errfn = function()
            actor:receive()
        end

        assert.has_error(errfn, "attempt to call method 'receive' (a nil value)")
    end)

    describe("A Lead Actor", function()
        it("is created from a regular actor, which closes its thread", function()
            actor:lead()
            actor:send{"move", 2, 2}
            actor:send{"move", 13, 13}
            assert.are.same({0, 0}, actor:scripts()[1]:probe("coordinates"))
        end)

        it("has to process its messages manually", function()
            actor:receive()
            assert.are.same({15, 15}, actor:scripts()[1]:probe("coordinates"))
        end)

        it("can be reloaded too", function()
            actor:load()
            assert.are.same({250, 250}, actor:scripts()[1]:probe("coordinates"))
        end)
    end)

end)

describe("A Dialogue", function()
    local dialogue = Dialogue.new{
        { {"weapon", "Crown", "North"} },
        {
            { 
                { {"draw", 2, 4} },
                {}
            },
            { 
                { {"draw", 400, 200} },
                {
                    { 
                        { {"weapon", "bullet", "south"} },
                        {}
                    },
                    { 
                        { {"weapon", "bomb", "south-east"} },
                        {}
                    }
                }
            },
            { 
                { {"draw", 20, 6} },
                {}
            }
        }
    }

    -- The 'form' of the tree:
    --    dialogue
    --     / | \
    --    a  b  e
    --      / \
    --     c   d
    
    local a = a
    local b = b
    local c = c
    local d = d
    local e = e

    it("is a tree of Actors", function()
        a = dialogue:children()[1]
        b = dialogue:children()[2]
        c = b:children()[1]
        d = b:children()[2]
        e = dialogue:children()[3]

        assert.are.same({a, b, e}, dialogue:children());
        assert.are.same({c, d}, b:children());
    end)

    pending("allows for scoping of messages through audience")

end)
