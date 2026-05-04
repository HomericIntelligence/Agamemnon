"""Tests for DAGWalker — cycle detection, ready tasks, available agents, and advance_dag."""
from __future__ import annotations

import asyncio
from unittest.mock import AsyncMock

import pytest

from agamemnon.orchestration.dag_walker import DAGWalker
from tests.orchestration.helpers import make_agent, make_task


class TestGetAvailableAgents:
    def test_online_active_agent_available(self) -> None:
        agent = make_agent()
        walker = DAGWalker(tasks=[], agents=[agent])
        assert agent in walker.get_available_agents()

    def test_busy_agent_not_available(self) -> None:
        agent = make_agent(current_task_id="some-task")
        walker = DAGWalker(tasks=[], agents=[agent])
        assert agent not in walker.get_available_agents()

    def test_inactive_agent_not_available(self) -> None:
        agent = make_agent(status="inactive")
        walker = DAGWalker(tasks=[], agents=[agent])
        assert agent not in walker.get_available_agents()

    def test_offline_agent_not_available(self) -> None:
        agent = make_agent(session_status="offline")
        walker = DAGWalker(tasks=[], agents=[agent])
        assert agent not in walker.get_available_agents()

    def test_mixed_agents_only_idle_returned(self) -> None:
        idle = make_agent(id="a1")
        busy = make_agent(id="a2", current_task_id="t-99")
        offline = make_agent(id="a3", session_status="offline")
        walker = DAGWalker(tasks=[], agents=[idle, busy, offline])
        result = walker.get_available_agents()
        assert result == [idle]


class TestGetReadyTasks:
    def test_task_with_no_deps_is_ready(self) -> None:
        task = make_task()
        walker = DAGWalker(tasks=[task], agents=[])
        assert task in walker.get_ready_tasks()

    def test_task_with_completed_dep_is_ready(self) -> None:
        dep = make_task(id="dep-1", status="completed")
        task = make_task(id="task-2", dependencies=["dep-1"])
        walker = DAGWalker(tasks=[dep, task], agents=[])
        assert task in walker.get_ready_tasks()

    def test_task_with_pending_dep_not_ready(self) -> None:
        dep = make_task(id="dep-1", status="pending")
        task = make_task(id="task-2", dependencies=["dep-1"])
        walker = DAGWalker(tasks=[dep, task], agents=[])
        assert task not in walker.get_ready_tasks()

    def test_already_assigned_task_not_ready(self) -> None:
        task = make_task(assigned_agent_id="agent-99")
        walker = DAGWalker(tasks=[task], agents=[])
        assert task not in walker.get_ready_tasks()


class TestValidateNoCycles:
    def test_empty_dag_is_acyclic(self) -> None:
        walker = DAGWalker(tasks=[], agents=[])
        assert walker.validate_no_cycles() is True

    def test_single_task_no_deps_is_acyclic(self) -> None:
        task = make_task(id="t1")
        walker = DAGWalker(tasks=[task], agents=[])
        assert walker.validate_no_cycles() is True

    def test_linear_chain_is_acyclic(self) -> None:
        # t1 -> t2 -> t3
        t1 = make_task(id="t1")
        t2 = make_task(id="t2", dependencies=["t1"])
        t3 = make_task(id="t3", dependencies=["t2"])
        walker = DAGWalker(tasks=[t1, t2, t3], agents=[])
        assert walker.validate_no_cycles() is True

    def test_diamond_dag_is_acyclic(self) -> None:
        # t1 -> t2, t1 -> t3, t2 -> t4, t3 -> t4
        t1 = make_task(id="t1")
        t2 = make_task(id="t2", dependencies=["t1"])
        t3 = make_task(id="t3", dependencies=["t1"])
        t4 = make_task(id="t4", dependencies=["t2", "t3"])
        walker = DAGWalker(tasks=[t1, t2, t3, t4], agents=[])
        assert walker.validate_no_cycles() is True

    def test_two_node_cycle_detected(self) -> None:
        # t1 -> t2 -> t1
        t1 = make_task(id="t1", dependencies=["t2"])
        t2 = make_task(id="t2", dependencies=["t1"])
        walker = DAGWalker(tasks=[t1, t2], agents=[])
        assert walker.validate_no_cycles() is False

    def test_three_node_cycle_detected(self) -> None:
        # t1 -> t2 -> t3 -> t1
        t1 = make_task(id="t1", dependencies=["t3"])
        t2 = make_task(id="t2", dependencies=["t1"])
        t3 = make_task(id="t3", dependencies=["t2"])
        walker = DAGWalker(tasks=[t1, t2, t3], agents=[])
        assert walker.validate_no_cycles() is False

    def test_self_cycle_detected(self) -> None:
        t1 = make_task(id="t1", dependencies=["t1"])
        walker = DAGWalker(tasks=[t1], agents=[])
        assert walker.validate_no_cycles() is False

    def test_cycle_in_subgraph_detected(self) -> None:
        # t1 is clean; t2 <-> t3 form a cycle
        t1 = make_task(id="t1")
        t2 = make_task(id="t2", dependencies=["t3"])
        t3 = make_task(id="t3", dependencies=["t2"])
        walker = DAGWalker(tasks=[t1, t2, t3], agents=[])
        assert walker.validate_no_cycles() is False

    def test_deep_chain_no_recursion_error(self) -> None:
        """2000-node linear chain must not raise RecursionError (regression for #100)."""
        n = 2000
        tasks = [make_task(id="t0")]
        for i in range(1, n):
            tasks.append(make_task(id=f"t{i}", dependencies=[f"t{i - 1}"]))
        walker = DAGWalker(tasks=tasks, agents=[])
        assert walker.validate_no_cycles() is True

    def test_external_dependency_raises(self) -> None:
        """Dependencies referencing task IDs not in the graph raise ValueError (Issue #233)."""
        t1 = make_task(id="t1", dependencies=["external-task-not-in-graph"])
        walker = DAGWalker(tasks=[t1], agents=[])
        with pytest.raises(ValueError, match="Unknown dependency"):
            walker.validate_no_cycles()

    def test_validate_no_cycles_reports_cycle_path(self) -> None:
        """_find_cycle_path() returns the task IDs involved in the cycle (Issue #229)."""
        # A -> B -> A forms a 2-node cycle
        t_a = make_task(id="A", dependencies=["B"])
        t_b = make_task(id="B", dependencies=["A"])
        walker = DAGWalker(tasks=[t_a, t_b], agents=[])
        cycle_path = walker._find_cycle_path()
        assert len(cycle_path) >= 2
        assert "A" in cycle_path
        assert "B" in cycle_path


class TestAdvanceDag:
    async def test_assigns_ready_task_to_available_agent(self) -> None:
        task = make_task()
        agent = make_agent()
        walker = DAGWalker(tasks=[task], agents=[agent])
        assignments = await walker.advance_dag()
        assert len(assignments) == 1
        assert assignments[0] == (task, agent)
        assert agent.current_task_id == task.id
        assert task.assigned_agent_id == agent.id

    async def test_multiple_assignments_use_different_agents(self) -> None:
        task1 = make_task(id="t1")
        task2 = make_task(id="t2")
        agent1 = make_agent(id="a1")
        agent2 = make_agent(id="a2")
        walker = DAGWalker(tasks=[task1, task2], agents=[agent1, agent2])
        assignments = await walker.advance_dag()
        assert len(assignments) == 2
        assigned_agents = {a.id for _, a in assignments}
        assert assigned_agents == {"a1", "a2"}

    async def test_busy_agent_not_double_assigned(self) -> None:
        """A single idle agent with two ready tasks should only get one assignment."""
        task1 = make_task(id="t1")
        task2 = make_task(id="t2")
        agent = make_agent(id="a1")
        walker = DAGWalker(tasks=[task1, task2], agents=[agent])
        assignments = await walker.advance_dag()
        assert len(assignments) == 1
        assert agent.current_task_id is not None

    async def test_agent_marked_busy_immediately(self) -> None:
        task1 = make_task(id="t1")
        task2 = make_task(id="t2")
        agent = make_agent(id="a1")
        walker = DAGWalker(tasks=[task1, task2], agents=[agent])
        await walker.advance_dag()
        assert sum(1 for t in walker.tasks if t.assigned_agent_id is not None) == 1

    async def test_no_available_agents_no_assignments(self) -> None:
        task = make_task()
        agent = make_agent(current_task_id="other-task")
        walker = DAGWalker(tasks=[task], agents=[agent])
        assignments = await walker.advance_dag()
        assert assignments == []

    async def test_no_ready_tasks_no_assignments(self) -> None:
        dep = make_task(id="dep", status="in_progress")
        task = make_task(id="t1", dependencies=["dep"])
        agent = make_agent()
        walker = DAGWalker(tasks=[dep, task], agents=[agent])
        assignments = await walker.advance_dag()
        assert assignments == []

    async def test_advance_dag_calls_client_assign_task(self) -> None:
        """advance_dag() must call client.assign_task(task_id, agent_id) when a client is set."""
        task = make_task()
        agent = make_agent()
        mock_client = AsyncMock()
        # Return the same agent list so the fresh-agent refresh (issue #196) is a no-op.
        mock_client.get_agents = AsyncMock(return_value=[agent])
        walker = DAGWalker(tasks=[task], agents=[agent], client=mock_client)

        assignments = await walker.advance_dag()

        assert len(assignments) == 1
        mock_client.assign_task.assert_awaited_once_with(task.id, agent.id)

    @pytest.mark.asyncio
    async def test_advance_dag_no_client_no_api_call(self) -> None:
        """advance_dag() must succeed and return assignments when no client is provided."""
        task = make_task()
        agent = make_agent()
        walker = DAGWalker(tasks=[task], agents=[agent], client=None)

        assignments = await walker.advance_dag()

        assert len(assignments) == 1
        assert assignments[0] == (task, agent)

    @pytest.mark.asyncio
    async def test_advance_dag_marks_agent_busy(self) -> None:
        """advance_dag() must set agent.current_task_id after assignment."""
        task = make_task()
        agent = make_agent()
        walker = DAGWalker(tasks=[task], agents=[agent])

        await walker.advance_dag()

        assert agent.current_task_id == task.id
        assert task.assigned_agent_id == agent.id

    @pytest.mark.asyncio
    async def test_advance_dag_no_ready_tasks_returns_empty_with_client(self) -> None:
        """advance_dag() must return [] and never call assign_task when no tasks are ready."""
        dep = make_task(id="dep", status="in_progress")
        task = make_task(id="t1", dependencies=["dep"])
        agent = make_agent()
        mock_client = AsyncMock()
        walker = DAGWalker(tasks=[dep, task], agents=[agent], client=mock_client)

        assignments = await walker.advance_dag()

        assert assignments == []
        mock_client.assign_task.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_advance_dag_client_called_for_each_assignment(self) -> None:
        """assign_task() must be called once per task-agent pair when multiple assignments occur."""
        task1 = make_task(id="t1")
        task2 = make_task(id="t2")
        agent1 = make_agent(id="a1")
        agent2 = make_agent(id="a2")
        mock_client = AsyncMock()
        # Return the same agents so the fresh-agent refresh (issue #196) is a no-op.
        mock_client.get_agents = AsyncMock(return_value=[agent1, agent2])
        walker = DAGWalker(
            tasks=[task1, task2], agents=[agent1, agent2], client=mock_client
        )

        assignments = await walker.advance_dag()

        assert len(assignments) == 2
        assert mock_client.assign_task.await_count == 2
        awaited_calls = {call.args for call in mock_client.assign_task.await_args_list}
        assert (task1.id, agent1.id) in awaited_calls or (
            task1.id,
            agent2.id,
        ) in awaited_calls
        assert (task2.id, agent1.id) in awaited_calls or (
            task2.id,
            agent2.id,
        ) in awaited_calls

    @pytest.mark.asyncio
    async def test_advance_dag_raises_on_cycle(self) -> None:
        """advance_dag() raises ValueError when the DAG contains a cycle (Issue #228)."""
        t_a = make_task(id="A", dependencies=["B"])
        t_b = make_task(id="B", dependencies=["A"])
        agent = make_agent()
        walker = DAGWalker(tasks=[t_a, t_b], agents=[agent])
        with pytest.raises(ValueError, match="Cycle detected in task DAG"):
            await walker.advance_dag()

    @pytest.mark.asyncio
    async def test_advance_dag_raises_on_unknown_dependency(self) -> None:
        """advance_dag() raises ValueError when a task references an unknown dep ID (Issue #233)."""
        task = make_task(id="t1", dependencies=["nonexistent-id"])
        agent = make_agent()
        walker = DAGWalker(tasks=[task], agents=[agent])
        with pytest.raises(ValueError, match="Unknown dependency"):
            await walker.advance_dag()


class TestAdvanceDagFreshAgents:
    """Tests for issue #196 — advance_dag() calls get_agents() for a fresh agent list."""

    async def test_advance_dag_calls_get_agents_when_available(self) -> None:
        """advance_dag() calls client.get_agents() and uses the returned list."""
        task = make_task()
        stale_agent = make_agent(id="stale", current_task_id="busy")
        fresh_agent = make_agent(id="fresh")
        mock_client = AsyncMock()
        mock_client.get_agents = AsyncMock(return_value=[fresh_agent])
        walker = DAGWalker(tasks=[task], agents=[stale_agent], client=mock_client)

        assignments = await walker.advance_dag()

        mock_client.get_agents.assert_awaited_once()
        assert len(assignments) == 1
        assert assignments[0][1].id == "fresh"

    async def test_advance_dag_skips_get_agents_when_no_client(self) -> None:
        """advance_dag() works without a client — uses self.agents unchanged."""
        task = make_task()
        agent = make_agent()
        walker = DAGWalker(tasks=[task], agents=[agent], client=None)

        assignments = await walker.advance_dag()

        assert len(assignments) == 1
        assert assignments[0][1] is agent

    async def test_advance_dag_skips_get_agents_when_client_lacks_method(self) -> None:
        """advance_dag() falls back to self.agents when client has no get_agents()."""
        from unittest.mock import MagicMock

        task = make_task()
        agent = make_agent()
        # Build a client that only has assign_task (an async callable) — no get_agents.
        mock_client = MagicMock()
        mock_client.assign_task = AsyncMock()
        del mock_client.get_agents  # ensure hasattr returns False
        walker = DAGWalker(tasks=[task], agents=[agent], client=mock_client)

        assignments = await walker.advance_dag()

        assert len(assignments) == 1
        assert assignments[0][1] is agent

    async def test_advance_dag_falls_back_on_get_agents_exception(self) -> None:
        """If get_agents() raises, advance_dag() falls back to the cached list."""
        task = make_task()
        cached_agent = make_agent(id="cached")
        mock_client = AsyncMock()
        mock_client.get_agents = AsyncMock(side_effect=RuntimeError("service down"))
        walker = DAGWalker(tasks=[task], agents=[cached_agent], client=mock_client)

        assignments = await walker.advance_dag()

        # Falls back to cached list — the cached agent was available
        assert len(assignments) == 1
        assert assignments[0][1].id == "cached"

    async def test_advance_dag_ignores_none_from_get_agents(self) -> None:
        """If get_agents() returns None, advance_dag() keeps the existing agent list."""
        task = make_task()
        cached_agent = make_agent(id="cached")
        mock_client = AsyncMock()
        mock_client.get_agents = AsyncMock(return_value=None)
        walker = DAGWalker(tasks=[task], agents=[cached_agent], client=mock_client)

        assignments = await walker.advance_dag()

        assert len(assignments) == 1
        assert assignments[0][1].id == "cached"


class TestBackgroundScan:
    """Tests for issue #98 — periodic background DAG scan safety net."""

    async def test_start_background_scan_returns_task(self) -> None:
        """start_background_scan() returns an asyncio.Task."""
        walker = DAGWalker(tasks=[], agents=[], scan_interval=0.05)
        stop_event = asyncio.Event()
        task = walker.start_background_scan(stop_event)
        assert isinstance(task, asyncio.Task)
        stop_event.set()
        await task

    async def test_background_scan_calls_advance_dag(self) -> None:
        """Background scan calls advance_dag() at least once per interval."""
        ready_task = make_task()
        agent = make_agent()
        walker = DAGWalker(tasks=[ready_task], agents=[agent], scan_interval=0.02)

        call_count = 0
        original_advance = walker.advance_dag

        async def counting_advance() -> list:
            nonlocal call_count
            call_count += 1
            return await original_advance()

        walker.advance_dag = counting_advance  # type: ignore[method-assign]

        stop_event = asyncio.Event()
        scan_task = walker.start_background_scan(stop_event)
        await asyncio.sleep(0.07)  # allow ~3 intervals to fire
        stop_event.set()
        await scan_task

        assert call_count >= 1

    async def test_background_scan_survives_advance_dag_exception(self) -> None:
        """A raised exception in advance_dag() must not terminate the scan loop."""
        walker = DAGWalker(tasks=[], agents=[], scan_interval=0.02)
        call_count = 0

        async def failing_advance() -> list:
            nonlocal call_count
            call_count += 1
            if call_count < 2:
                raise RuntimeError("transient failure")
            return []

        walker.advance_dag = failing_advance  # type: ignore[method-assign]

        stop_event = asyncio.Event()
        scan_task = walker.start_background_scan(stop_event)
        await asyncio.sleep(0.07)
        stop_event.set()
        await scan_task

        assert call_count >= 2

    async def test_background_scan_exits_immediately_on_stop(self) -> None:
        """If stop_event is pre-set, the loop exits before calling advance_dag()."""
        walker = DAGWalker(tasks=[], agents=[], scan_interval=60.0)
        call_count = 0

        async def counting_advance() -> list:
            nonlocal call_count
            call_count += 1
            return []

        walker.advance_dag = counting_advance  # type: ignore[method-assign]

        stop_event = asyncio.Event()
        stop_event.set()  # pre-set before starting
        scan_task = walker.start_background_scan(stop_event)
        await scan_task

        assert call_count == 0

    async def test_start_background_scan_stores_task_reference(self) -> None:
        """start_background_scan() stores the task in self._scan_task."""
        walker = DAGWalker(tasks=[], agents=[], scan_interval=0.05)
        stop_event = asyncio.Event()
        task = walker.start_background_scan(stop_event)
        assert walker._scan_task is task
        stop_event.set()
        await task


class TestFindCyclePath:
    """Tests for _find_cycle_path() cycle detection with back-edge reconstruction."""

    def test_find_cycle_path_returns_empty_for_acyclic_graph(self) -> None:
        """_find_cycle_path() returns [] for an acyclic DAG."""
        t1 = make_task(id="t1")
        t2 = make_task(id="t2", dependencies=["t1"])
        walker = DAGWalker(tasks=[t1, t2], agents=[])
        assert walker._find_cycle_path() == []

    def test_find_cycle_path_detects_immediate_back_edge(self) -> None:
        """_find_cycle_path() detects immediate back-edge to GRAY neighbor (line 108-117)."""
        # t1 -> t2, t2 -> t1 (immediate back-edge when processing t2's edges)
        t1 = make_task(id="t1", dependencies=["t2"])
        t2 = make_task(id="t2", dependencies=["t1"])
        walker = DAGWalker(tasks=[t1, t2], agents=[])
        cycle = walker._find_cycle_path()
        assert len(cycle) >= 2
        assert "t1" in cycle
        assert "t2" in cycle

    def test_find_cycle_path_detects_deferred_back_edge(self) -> None:
        """_find_cycle_path() detects deferred back-edge when revisiting GRAY node (line 91-100)."""
        # t1 -> t2, t2 -> t3, t3 -> t1 (back-edge found when popping t1 from stack)
        t1 = make_task(id="t1", dependencies=["t3"])
        t2 = make_task(id="t2", dependencies=["t1"])
        t3 = make_task(id="t3", dependencies=["t2"])
        walker = DAGWalker(tasks=[t1, t2, t3], agents=[])
        cycle = walker._find_cycle_path()
        assert len(cycle) >= 2
        assert "t1" in cycle
        assert "t2" in cycle
        assert "t3" in cycle

    def test_find_cycle_path_handles_black_node_skip(self) -> None:
        """_find_cycle_path() skips BLACK (already visited) nodes (line 102-103)."""
        # Complex graph: t1 -> t2, t1 -> t3, t2 -> t3, t3 -> t2 (cycle in t2 <-> t3)
        t1 = make_task(id="t1")
        t2 = make_task(id="t2", dependencies=["t3"])
        t3 = make_task(id="t3", dependencies=["t2"])
        walker = DAGWalker(tasks=[t1, t2, t3], agents=[])
        cycle = walker._find_cycle_path()
        # t1 is visited first (no cycle), then t2 <-> t3 is detected
        assert "t2" in cycle
        assert "t3" in cycle

    def test_find_cycle_path_with_multiple_independent_components(self) -> None:
        """_find_cycle_path() detects cycle in one component (line 75-77 loop)."""
        # Component 1: t1 -> t2 (no cycle)
        # Component 2: t3 -> t4 -> t3 (cycle)
        t1 = make_task(id="t1")
        t2 = make_task(id="t2", dependencies=["t1"])
        t3 = make_task(id="t3", dependencies=["t4"])
        t4 = make_task(id="t4", dependencies=["t3"])
        walker = DAGWalker(tasks=[t1, t2, t3, t4], agents=[])
        cycle = walker._find_cycle_path()
        # Should find the cycle in component 2
        assert "t3" in cycle
        assert "t4" in cycle
