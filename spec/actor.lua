_G.arg = {}
require 'busted.runner'()

describe("An Actor reference object", function()
    local a0 = nil
    local a1 = nil

    --
    -- TODO: In the future these Actor tests are going to need to happen to an
    -- actor restricted to the main thread.
    --

    it("will error on creation if not given correct Script definitions", function()
        assert.has_error(function() 
            Actor{ {"good"}, "bad" }
        end, "Failed to create script: `bad` isn't a table!")

        assert.has_error(function() 
            Actor{ {"good"}, {}, {"another good"} }
        end, "Failed to create script: invalid definition!")
    end)

    it("can be created from a definition of Scripts", function()
        a0 = Actor{ {"draw", 200, 400} }
        assert.is_equal(a0:id(), 0)
    end)

    it("can be created by providing the id of an existing Actor", function()
        a0 = nil
        a0 = Actor(0)
        assert.is_equal(a0:id(), 0)
    end)

    it("won't error on creation if provided with a bad integer id", function()
        a1 = Actor(20)
    end)

    it("will error on load if the actor id is invalid", function()
        assert.has_error(function() 
            a1:load()
        end, "Actor id `20` is an invalid reference!")
    end)

    it("will error on load if the any Script has an invalid module", function()
        a1 = nil
        a1 = Actor{ {"invalid-module"} }

        assert.has_error(function() 
            a1:load()
        end, "Cannot load module `invalid-module': require failed")

        a1:delete()
    end)

    it("will error on load if the any Script has a module with no new function", function()
        a1 = nil
        a1 = Actor{ {"module-no-new"} }

        assert.has_error(function() 
            a1:load()
        end, "Cannot load module `module-no-new': `new' is not a function!")

        a1:delete()
    end)

    it("cannot be sent a message or probed if not loaded", function()
        assert.has_error(function() 
            a0:probe(1, "coordinates")
        end, "Cannot probe `coordinates': not loaded!")

        -- Will only throw this error if there are no loaded Scripts -- as in
        -- if there's 1 out of 50 loaded Scripts, it won't throw an error.
        assert.has_error(function() 
            a0:send{"move", 2, 2}
        end, "Actor `0' has no loaded Scripts!")
    end)

    it("can load (or reload) any Scripts an Actor might have", function()
        a0:load()
    end)

    it("will error on any function with a bad message format", function()
        assert.has_error(function() 
            a0:send{"move"}
        end, "attempt to perform arithmetic on local 'x' (a nil value)")
    end)
    
    it("will unload any Scripts that have errored", function()
        assert.has_error(function() 
            a0:send{"move", 2, 2}
        end, "Actor `0' has no loaded Scripts!")

        -- call load to reload all unloaded scripts
        a0:load(1)
        a0:load("all")
    end)

    it("can be sent messages which affects the real Actor's state", function()
        -- a0 is set at 200, 400 when it is created
        a0:send{"move", 2, 2}
        assert.are_same({202, 402}, a0:probe(1, "coordinates"))
    end)

    pending("can be given a name (string) to reference the Actor just like an id")
    pending("can be created by providing the name of an existing Actor")
end)
