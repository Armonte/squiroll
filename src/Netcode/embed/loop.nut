this.env_stack <- [];
this.update_func <- null;
this.task <- {};
this.task_async <- {};
this.pause_count <- 0;
class this.GeneratorTask
{
	gen = null;
	function Set( _gen )
	{
		this.gen = _gen;
		::loop.AddTask(this);
	}

	function Reset()
	{
		this.gen = null;
		::loop.DeleteTask(this);
	}

	function Update()
	{
		if (resume this.gen)
		{
			return;
		}

		::loop.DeleteTask(this);
	}

}

function Begin( env )
{
	if ("Update" in env)
	{
		this.env_stack.append(env);
	}

	this.pause_count = 0;
}

function End( scene = null )
{
	if (scene)
	{
		if (this.env_stack.top() == scene)
		{
			return;
		}

		while (this.env_stack.len() > 0)
		{
			if ("Terminate" in this.env_stack.top())
			{
				this.env_stack.top().Terminate();
			}

			this.env_stack.pop();

			if (this.env_stack.top() == scene)
			{
				break;
			}
		}
	}
	else if (this.env_stack.len() > 0)
	{
		if ("Terminate" in this.env_stack.top())
		{
			this.env_stack.top().Terminate();
		}

		this.env_stack.pop();
	}

	if (this.env_stack.len())
	{
		local env = this.env_stack.top();

		if ("Resume" in env)
		{
			env.Resume.call(env);
		}

		if ("Update" in env)
		{
			this.update_func = env.Update;
		}
	}
	else
	{
		this.update_func = null;
	}
}

function Move( env )
{
	if (this.env_stack.len())
	{
		if ("Terminate" in this.env_stack.top())
		{
			this.env_stack.top().Terminate();
		}

		this.env_stack.pop();
	}

	if ("Update" in env)
	{
		this.env_stack.append(env);
	}

	this.pause_count = 0;
}

function Update()
{
	::input_all.Update();
	// Gekko rollback: once the gekko session owns the frame loop, the
	// scene must advance EXACTLY ONCE per engine frame, ungated. The
	// vanilla netplay branch below runs the scene inside
	// `while (::network.inst.SyncInput())` — SyncInput() is the
	// DELAY-BASED netcode's "is the peer's input ready" check. Under
	// gekko, inputs are supplied by gekko (injected into the recorder),
	// not the delay-based stream; SyncInput() returns false for the
	// first ~10 frames on the client (its delay-based stream primes
	// asymmetrically vs the host) and the battle stalls -> the two
	// peers' engine `count` drift apart -> desync. Bypass the gate.
	local gekko_on = ("network" in ::setting)
	              && ("gekko_session_started" in ::setting.network)
	              && ::setting.network.gekko_session_started();

	if (this.pause_count > 0)
	{
		this.pause_count--;
	}
	else if (gekko_on)
	{
		if (this.env_stack.len() > 0)
		{
			this.env_stack.top().Update();
		}

		foreach( v in this.task )
		{
			v.Update();
		}
	}
	else if (::network.IsPlaying() && ::network.ready && !::network.request)
	{
		if (::network.input_local)
		{
			::network.input_local.Update();
		}

		while (::network.inst && ::network.inst.SyncInput())
		{
			if (this.env_stack.len() > 0)
			{
				this.env_stack.top().Update();
			}

			foreach( v in this.task )
			{
				v.Update();
			}

			if (this.pause_count > 0)
			{
				break;
			}
		}
	}
	else
	{
		//::rollback.preframe();

		if (this.env_stack.len() > 0)
		{
			this.env_stack.top().Update();
		}

		foreach( v in this.task )
		{
			v.Update();
		}

		//::rollback.postframe();
	}

    local now = ::manbow.timeGetTime();
    local i = 0;
	foreach(v in this.task_async) {
        v.Update();
	    local n = ::manbow.timeGetTime();
        //::print(::format("task %d took %d ms\n", i,n - now));
        i++;
        now = n;
    }
}

function Pause( count )
{
	this.pause_count = count;
}

function AddTask( actor, sync = false )
{
	if (sync)
	{
		if (actor.tostring() in this.task_async)
		{
			delete this.task_async[actor.tostring()];
		}

		this.task[actor.tostring()] <- actor;
	}
	else
	{
		if (actor.tostring() in this.task)
		{
			delete this.task[actor.tostring()];
		}

		this.task_async[actor.tostring()] <- actor;
	}
}

function DeleteTask( actor )
{
	if (actor.tostring() in this.task)
	{
		delete this.task[actor.tostring()];
	}
	else if (actor.tostring() in this.task_async)
	{
		delete this.task_async[actor.tostring()];
	}
}

function Fade( callback, count = 30, r = 0, g = 0, b = 0 )
{
	local t = {};
	t.callback <- callback;
	t.count_base <- count;
	t.count <- count;
	t.Update <- function ()
	{
		count = count;

		if (count-- == 0)
		{
			::loop.DeleteTask(this);
			callback();
			::graphics.FadeIn(this.count_base);
			::loop.Pause(3);
		}
	};
	this.Pause(count + 1);
	this.AddTask(t);
	::graphics.FadeOut(count, null, r, g, b);
}

function EndWithFade()
{
	this.Fade(function ()
	{
		::loop.End();
	});
}

function GetCurrentScene()
{
	return this.env_stack.top();
}

::manbow.SetUpdateFunction(function ()
{
	::loop.Update();
});
