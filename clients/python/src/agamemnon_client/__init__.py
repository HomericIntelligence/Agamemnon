"""agamemnon-client: async Python client for the Agamemnon REST API."""

from __future__ import annotations

from importlib.metadata import PackageNotFoundError, version

from agamemnon_client.client import AgamemnonClient as AgamemnonClient
from agamemnon_client.config import Settings as Settings
from agamemnon_client.config import load_settings as load_settings
from agamemnon_client.errors import AgamemnonAPIError as AgamemnonAPIError
from agamemnon_client.errors import AgamemnonConnectionError as AgamemnonConnectionError
from agamemnon_client.errors import AgamemnonError as AgamemnonError
from agamemnon_client.models import AgamemnonConfig as AgamemnonConfig
from agamemnon_client.models import Agent as Agent
from agamemnon_client.models import AgentCreate as AgentCreate
from agamemnon_client.models import AgentDockerCreate as AgentDockerCreate
from agamemnon_client.models import AgentUpdate as AgentUpdate
from agamemnon_client.models import ChaosEntry as ChaosEntry
from agamemnon_client.models import FailureSpec as FailureSpec
from agamemnon_client.models import HealthResponse as HealthResponse
from agamemnon_client.models import InjectionResult as InjectionResult
from agamemnon_client.models import Task as Task
from agamemnon_client.models import TaskCreate as TaskCreate
from agamemnon_client.models import TaskUpdate as TaskUpdate
from agamemnon_client.models import Team as Team
from agamemnon_client.models import TeamCreate as TeamCreate
from agamemnon_client.models import TeamUpdate as TeamUpdate
from agamemnon_client.models import VersionResponse as VersionResponse

try:
    __version__ = version("HomericIntelligence-Agamemnon")
except PackageNotFoundError:
    __version__ = "0.0.0.dev0"

__all__ = [
    "AgamemnonClient",
    "AgamemnonError",
    "AgamemnonConnectionError",
    "AgamemnonAPIError",
    "AgamemnonConfig",
    "Agent",
    "AgentCreate",
    "AgentDockerCreate",
    "AgentUpdate",
    "Team",
    "TeamCreate",
    "TeamUpdate",
    "Task",
    "TaskCreate",
    "TaskUpdate",
    "HealthResponse",
    "VersionResponse",
    "FailureSpec",
    "InjectionResult",
    "ChaosEntry",
    "Settings",
    "load_settings",
]
