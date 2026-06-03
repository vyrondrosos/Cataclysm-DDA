#!/usr/bin/env python3
"""Generate sweep tables for mortar and FPV drone support tuning.

This mirrors the current C++ constants and formulas in src/mortar.cpp and
src/npctalk.cpp closely enough for balance exercises.  It is intentionally
deterministic: repeat mortar rows use expected correction improvement from the
spotting probability rather than sampling random impacts.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Iterable, Sequence


MORTAR_RANGE_ERROR_RATIO = 0.015
MORTAR_DEFLECTION_ERROR_MILS = 2.0
MORTAR_MINIMUM_LAUNCHER_SKILL = 4
MORTAR_MIN_SKILL_ERROR_MULTIPLIER = 3.0
MORTAR_MULTIPLIER_SOFT_CAP_THRESHOLD = 10.0
MORTAR_MULTIPLIER_HARD_CAP = 70.0
MORTAR_MULTIPLIER_ABOVE_SOFT_CAP_SCALE = 0.5
MORTAR_WEATHER_ERROR_MULTIPLIER = 3.0
MORTAR_NO_TACTICAL_DATA_ERROR_MULTIPLIER = 3.0
MORTAR_NO_PROFICIENCY_ERROR_MULTIPLIER = 4.0
MORTAR_BINOCULAR_REFERENCE_MULTIPLIER = 1.5
MORTAR_LASER_RANGEFINDER_SENSOR_MULTIPLIER = 1.8
MORTAR_LASER_RANGEFINDER_AXIS_MULTIPLIER = 0.5
MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER = 0.35
MORTAR_EPLRS_LOCATION_MULTIPLIER = 0.3
MORTAR_DRONE_SPOTTER_SENSOR_MULTIPLIER = 1.6
MORTAR_DRONE_DETECTION_RANGE_MULTIPLIER = 2.0
MORTAR_NO_WAIT_FEEDBACK_REDUCTION_SCALE = 0.5
MORTAR_LASER_RANGEFINDER_RANGE = 2000
MORTAR_ACERM_MAX_RANGE = 20000
MORTAR_CREW_SECONDARY_SKILL_WEIGHT = 0.35
MORTAR_CREW_SECONDARY_SKILL_CAP_MULTIPLIER = 1.3
MORTAR_GUIDED_NEAR_THRESHOLD_TILES = 100.0
MORTAR_GUIDED_NEAR_MULTIPLIER = 0.2
MORTAR_GUIDED_FAR_MULTIPLIER = 0.5
MORTAR_GUIDED_MIN_AXIS_TILES = 1.0

FPV_RESERVE_SECONDS = 20
FPV_STATION_BATTERY_RATE = 0.6


@dataclass(frozen=True)
class FeedbackObserver:
    sensor_multiplier: float
    detection_range_multiplier: float = 1.0
    repeat_location_multiplier: float = 0.5
    spotter_distance_tiles: float | None = None


@dataclass(frozen=True)
class SpotterProfile:
    key: str
    label: str
    sensor_multiplier: float
    rangefinder: bool = False
    eplrs_net: bool = False
    drone: bool = False
    detection_range_multiplier: float = 1.0
    repeat_location_multiplier: float = 0.5
    spotter_distance_tiles: float | None = None
    feedback_observers: tuple[FeedbackObserver, ...] = ()


@dataclass(frozen=True)
class MortarRow:
    profile: str
    range_tiles: int
    launcher_skill: int
    perception: int
    shot_index: int
    fixed_multiplier: float
    current_skill_multiplier: float
    total_multiplier: float
    ballistic_range_tiles: float
    ballistic_deflection_tiles: float
    location_range_tiles: float
    location_deflection_tiles: float
    total_range_tiles: float
    total_deflection_tiles: float
    shot_lost_pct: float
    feedback_accuracy_multiplier: float
    feedback_location_multiplier: float


@dataclass(frozen=True)
class CreepingRow:
    profile: str
    range_tiles: int
    launcher_skill: int
    perception: int
    shot_index: int
    total_range_tiles: float
    total_deflection_tiles: float
    player_offset_x_tiles: float
    player_offset_y_tiles: float
    creep_offset_x_tiles: int
    creep_offset_y_tiles: int
    creep_distance_tiles: float
    heading_degrees: int
    danger_close: bool
    offset_multiplier: float


@dataclass(frozen=True)
class DroneRow:
    drone_type: str
    distance_tiles: int
    driving_skill: int
    light: str
    launchable: str
    eta_s: int
    station_s: int
    return_s: int
    one_way_station_s: int
    scout_delay_s: int | str
    attack_fixed_cep_tiles: float | str
    attack_creature_cep_tiles: float | str
    attack_time_s: int | str
    drop_cep_tiles: float | str
    drop_delay_s: str
    mortar_spotter_location_cep_tiles: float | str
    mortar_spotter_lost_pct: float | str


@dataclass(frozen=True)
class MortarSpec:
    key: str
    label: str
    ammo_type: str
    range_tiles: int
    fire_delay_s: int
    deflection_error_mils: float
    max_secondary_crew: int


@dataclass(frozen=True)
class RoundSpec:
    key: str
    label: str
    mortar_key: str
    guidance: str
    max_range_tiles: int | None = None
    requirement: str = "none"
    target_rule: str = "tile or creature"
    mass_kg: float | None = None
    volume_l: float | None = None
    price_usd: int | None = None


MORTAR_SPECS: dict[str, MortarSpec] = {
    "m224": MortarSpec(
        "m224", "M224 60mm", "mortar_60mm", 3500, 10, 2.0, 1
    ),
    "m252": MortarSpec(
        "m252", "M252 81mm", "mortar_81mm", 5900, 20, 10.0, 2
    ),
}


ROUND_SPECS: dict[str, RoundSpec] = {
    "60mm_he_m720": RoundSpec(
        "60mm_he_m720", "M720A1 60mm HE", "m224", "unguided",
        mass_kg=1.700, volume_l=0.680, price_usd=400,
    ),
    "60mm_he_m768": RoundSpec(
        "60mm_he_m768", "M768 60mm HE", "m224", "unguided",
        mass_kg=1.655, volume_l=0.680, price_usd=600,
    ),
    "60mm_illum_m721": RoundSpec(
        "60mm_illum_m721", "M721 60mm illum", "m224", "unguided",
        mass_kg=1.700, volume_l=0.680, price_usd=350,
    ),
    "81mm_he_m821": RoundSpec(
        "81mm_he_m821", "M821A2 81mm HE", "m252", "unguided",
        mass_kg=4.273, volume_l=1.700, price_usd=900,
    ),
    "81mm_he_m889": RoundSpec(
        "81mm_he_m889", "M889A1 81mm HE", "m252", "unguided",
        mass_kg=4.273, volume_l=1.700, price_usd=850,
    ),
    "81mm_oksi_m821": RoundSpec(
        "81mm_oksi_m821", "OKSI M821A2 81mm HE", "m252", "oksi",
        requirement="active drone fixed-point designation",
        target_rule="fixed tile only",
        mass_kg=4.273, volume_l=1.700, price_usd=4900,
    ),
    "81mm_oksi_m889": RoundSpec(
        "81mm_oksi_m889", "OKSI M889A1 81mm HE", "m252", "oksi",
        requirement="active drone fixed-point designation",
        target_rule="fixed tile only",
        mass_kg=4.273, volume_l=1.700, price_usd=4850,
    ),
    "81mm_acerm": RoundSpec(
        "81mm_acerm", "ACERM 81mm HE", "m252", "acerm",
        max_range_tiles=MORTAR_ACERM_MAX_RANGE,
        requirement="active successful SOFLAM laser designation",
        target_rule="tile, immobile, or tracked creature",
        mass_kg=4.600, volume_l=2.200, price_usd=18000,
    ),
    "81mm_smoke_m819": RoundSpec(
        "81mm_smoke_m819", "M819 81mm smoke", "m252", "unguided",
        mass_kg=4.899, volume_l=2.200, price_usd=700,
    ),
    "81mm_illum_m853": RoundSpec(
        "81mm_illum_m853", "M853A1 81mm illum", "m252", "unguided",
        mass_kg=4.277, volume_l=2.200, price_usd=750,
    ),
}


def clamp(value: float, lower: float, upper: float) -> float:
    return min(upper, max(lower, value))


def parse_int_list(value: str) -> list[int]:
    return [int(part.strip()) for part in value.split(",") if part.strip()]


def parse_str_list(value: str) -> list[str]:
    return [part.strip() for part in value.split(",") if part.strip()]


def ns(**kwargs: object) -> SimpleNamespace:
    return SimpleNamespace(**kwargs)


def selected_mortars(keys: str) -> list[MortarSpec]:
    return [MORTAR_SPECS[key] for key in parse_str_list(keys)]


def selected_rounds(keys: str) -> list[RoundSpec]:
    return [ROUND_SPECS[key] for key in parse_str_list(keys)]


def skill_accuracy_multiplier(launcher_skill: int) -> float:
    skill = clamp(float(launcher_skill), MORTAR_MINIMUM_LAUNCHER_SKILL, 10.0)
    return 1.0 + (10.0 - skill) * (
        (MORTAR_MIN_SKILL_ERROR_MULTIPLIER - 1.0)
        / (10.0 - MORTAR_MINIMUM_LAUNCHER_SKILL)
    )


def effective_ballistic_multiplier(raw_multiplier: float) -> float:
    if raw_multiplier <= MORTAR_MULTIPLIER_SOFT_CAP_THRESHOLD:
        return max(1.0, raw_multiplier)
    return min(
        MORTAR_MULTIPLIER_HARD_CAP,
        MORTAR_MULTIPLIER_SOFT_CAP_THRESHOLD
        + (raw_multiplier - MORTAR_MULTIPLIER_SOFT_CAP_THRESHOLD)
        * MORTAR_MULTIPLIER_ABOVE_SOFT_CAP_SCALE,
    )


def fixed_accuracy_multiplier(
    has_proficiency: bool, has_tactical_data: bool, first_shot: bool
) -> float:
    proficiency = 1.0 if has_proficiency else MORTAR_NO_PROFICIENCY_ERROR_MULTIPLIER
    tactical = 1.0 if has_tactical_data else MORTAR_NO_TACTICAL_DATA_ERROR_MULTIPLIER
    weather = MORTAR_WEATHER_ERROR_MULTIPLIER if first_shot else 1.0
    return proficiency * tactical * weather


def repeat_cep_multiplier(launcher_skill: int) -> float:
    skill = clamp(float(launcher_skill), 1.0, 10.0)
    if skill <= 2.0:
        return 0.7 + (2.0 - skill) * 0.05
    if skill <= 5.0:
        return 0.7 - (skill - 2.0) * (0.2 / 3.0)
    return 0.5 - (skill - 5.0) * (0.2 / 5.0)


def soft_cap_sensor_multiplier(multiplier: float) -> float:
    if multiplier <= 4.0:
        return multiplier
    return 4.0 + (multiplier - 4.0) / (1.0 + multiplier - 4.0)


def perception_spotting_factor(perception: int) -> float:
    if perception <= 1:
        return 0.1
    if perception <= 5:
        return 0.1 + (perception - 1) * (0.4 / 4.0)
    return min(1.0, 0.5 + (perception - 5) * (0.5 / 5.0))


def repeat_accuracy_feedback_multiplier(
    launcher_skill: int, perception: int, sensor_multiplier: float
) -> float:
    base_reduction = 1.0 - repeat_cep_multiplier(launcher_skill)
    reduction = clamp(
        base_reduction * perception_spotting_factor(perception) * sensor_multiplier,
        0.0,
        0.95,
    )
    return 1.0 - reduction


def feedback_multiplier(multiplier: float, no_wait_adjustment: bool) -> float:
    clamped = clamp(multiplier, 0.0, 1.0)
    if not no_wait_adjustment:
        return clamped
    return 1.0 - (1.0 - clamped) * MORTAR_NO_WAIT_FEEDBACK_REDUCTION_SCALE


def base_location_error(perception: int, sensor_multiplier: float) -> float:
    per = clamp(float(perception), 1.0, 10.0)
    if per <= 5.0:
        base = 500.0 - (per - 1.0) * (250.0 / 4.0)
    else:
        base = 250.0 - (per - 5.0) * (100.0 / 5.0)
    return max(1.0, base * MORTAR_BINOCULAR_REFERENCE_MULTIPLIER / sensor_multiplier)


def minimum_range_error(distance: int) -> float:
    return max(1.0, distance * MORTAR_RANGE_ERROR_RATIO)


def minimum_deflection_error(distance: int, deflection_error_mils: float = MORTAR_DEFLECTION_ERROR_MILS) -> float:
    return max(1.0, distance * deflection_error_mils / 1000.0)


def compatible_rounds_for_mortar(rounds: Sequence[RoundSpec], mortar: MortarSpec) -> list[RoundSpec]:
    return [round_spec for round_spec in rounds if round_spec.mortar_key == mortar.key]


def round_max_range(round_spec: RoundSpec) -> int:
    mortar = MORTAR_SPECS[round_spec.mortar_key]
    return round_spec.max_range_tiles if round_spec.max_range_tiles is not None else mortar.range_tiles


def mortar_60mm_flight_time_bounds(distance: int) -> tuple[int, int]:
    clamped_distance = max(0, distance)
    if clamped_distance <= 500:
        return (10, 20)
    if clamped_distance <= 1000:
        return (
            round(10 + (15 - 10) * (clamped_distance - 500) / 500),
            round(20 + (25 - 20) * (clamped_distance - 500) / 500),
        )
    if clamped_distance <= 2000:
        return (
            round(15 + (25 - 15) * (clamped_distance - 1000) / 1000),
            round(25 + (40 - 25) * (clamped_distance - 1000) / 1000),
        )
    if clamped_distance <= 3000:
        return (
            round(25 + (35 - 25) * (clamped_distance - 2000) / 1000),
            round(40 + (50 - 40) * (clamped_distance - 2000) / 1000),
        )
    return (35, 50)


def expected_base_flight_seconds(mortar: MortarSpec, distance: int) -> float:
    range_scale = max(0.1, mortar.range_tiles / 3500.0)
    equivalent_60mm_distance = round(distance / range_scale)
    lower, upper = mortar_60mm_flight_time_bounds(equivalent_60mm_distance)
    return ((lower + upper) / 2.0) * math.sqrt(range_scale)


def expected_round_flight_seconds(mortar: MortarSpec, round_spec: RoundSpec, distance: int) -> float:
    if round_spec.guidance != "acerm" or distance <= mortar.range_tiles:
        return expected_base_flight_seconds(mortar, distance)
    extra_range = max(1, MORTAR_ACERM_MAX_RANGE - mortar.range_tiles)
    fraction = clamp((distance - mortar.range_tiles) / extra_range, 0.0, 1.0)
    return expected_base_flight_seconds(mortar, mortar.range_tiles) + 60.0 * fraction * fraction


def secondary_skills_for_count(count: int, secondary_skill: int) -> tuple[int, ...]:
    return tuple(secondary_skill for _ in range(count))


def crew_effective_launcher_skill(primary_skill: int, secondary_skills: Sequence[int]) -> float:
    weighted_skill_square = float(primary_skill * primary_skill)
    for secondary_skill in secondary_skills:
        capped = min(float(secondary_skill), primary_skill * MORTAR_CREW_SECONDARY_SKILL_CAP_MULTIPLIER)
        weighted_skill_square += MORTAR_CREW_SECONDARY_SKILL_WEIGHT * capped * capped
    return clamp(math.sqrt(weighted_skill_square), float(primary_skill), 10.0)


def crew_adjusted_fire_delay(mortar: MortarSpec, crew_count: int) -> int:
    return max(1, mortar.fire_delay_s // (1 << crew_count) - crew_count * 2)


def round_accuracy_distance(mortar: MortarSpec, round_spec: RoundSpec, distance: int) -> int:
    if round_spec.guidance != "acerm" or distance <= mortar.range_tiles:
        return distance
    extra_range = max(1, round_max_range(round_spec) - mortar.range_tiles)
    fraction = clamp((distance - mortar.range_tiles) / extra_range, 0.0, 1.0)
    return round(mortar.range_tiles * (1.0 + 0.5 * fraction))


def guided_axis_error(axis_error: float) -> float:
    if axis_error <= 0.0:
        return 0.0
    if axis_error <= MORTAR_GUIDED_NEAR_THRESHOLD_TILES:
        return max(MORTAR_GUIDED_MIN_AXIS_TILES, axis_error * MORTAR_GUIDED_NEAR_MULTIPLIER)
    guided = (
        MORTAR_GUIDED_NEAR_THRESHOLD_TILES * MORTAR_GUIDED_NEAR_MULTIPLIER
        + (axis_error - MORTAR_GUIDED_NEAR_THRESHOLD_TILES) * MORTAR_GUIDED_FAR_MULTIPLIER
    )
    return max(MORTAR_GUIDED_MIN_AXIS_TILES, guided)


def designation_success_probability(perception: int, dexterity: int, target_speed: int, moving: bool, tile: bool) -> float:
    if tile or target_speed <= 0:
        return 1.0
    effective_speed = float(target_speed if moving else max(1, target_speed) / 4.0)
    return clamp((perception * dexterity / effective_speed) * 2.0, 0.0, 1.0)


def effective_profile_sensor(profile: SpotterProfile, distance: int) -> float:
    if profile.drone:
        return profile.sensor_multiplier
    if profile.rangefinder and distance <= MORTAR_LASER_RANGEFINDER_RANGE:
        return profile.sensor_multiplier
    if profile.rangefinder:
        return MORTAR_BINOCULAR_REFERENCE_MULTIPLIER
    return profile.sensor_multiplier


def effective_repeat_location_multiplier(profile: SpotterProfile, distance: int) -> float:
    repeat = profile.repeat_location_multiplier
    if profile.rangefinder and distance > MORTAR_LASER_RANGEFINDER_RANGE:
        repeat = 0.5
    if profile.eplrs_net:
        repeat *= MORTAR_EPLRS_LOCATION_MULTIPLIER
    return repeat


def project_location_error(
    location_range: float, location_deflection: float, axis_angle_degrees: float
) -> tuple[float, float]:
    angle = math.radians(axis_angle_degrees)
    cos_angle = abs(math.cos(angle))
    sin_angle = abs(math.sin(angle))
    return (
        math.hypot(location_range * cos_angle, location_deflection * sin_angle),
        math.hypot(location_range * sin_angle, location_deflection * cos_angle),
    )


def axis_unit(axis_from: tuple[float, float], axis_to: tuple[float, float]) -> tuple[float, float]:
    ux = axis_to[0] - axis_from[0]
    uy = axis_to[1] - axis_from[1]
    length = math.hypot(ux, uy)
    if length <= 0.0:
        return (1.0, 0.0)
    return (ux / length, uy / length)


def heading_degrees(origin: tuple[float, float], target: tuple[float, float]) -> int:
    degrees = round(math.atan2(target[0] - origin[0], origin[1] - target[1]) * 180.0 / math.pi)
    if degrees < 0:
        degrees += 360
    return int(degrees)


def elliptic_distance(
    axis_from: tuple[float, float],
    axis_to: tuple[float, float],
    center: tuple[float, float],
    point: tuple[float, float],
    range_error: float,
    deflection_error: float,
) -> float:
    ux, uy = axis_unit(axis_from, axis_to)
    dx = point[0] - center[0]
    dy = point[1] - center[1]
    range_offset = dx * ux + dy * uy
    deflection_offset = -dx * uy + dy * ux
    return math.hypot(
        range_offset / max(1.0, range_error),
        deflection_offset / max(1.0, deflection_error),
    )


def make_creeping_axis_to(
    target: tuple[float, float], spotter_pos: tuple[float, float], mortar_pos: tuple[float, float]
) -> tuple[float, float]:
    dx = target[0] - spotter_pos[0]
    dy = target[1] - spotter_pos[1]
    length = math.hypot(dx, dy)
    if length <= 0.0:
        dx = target[0] - mortar_pos[0]
        dy = target[1] - mortar_pos[1]
        length = math.hypot(dx, dy)
    if length <= 0.0:
        dx = 1.0
        dy = 0.0
        length = 1.0
    return (target[0] + round(dx / length * 1000.0), target[1] + round(dy / length * 1000.0))


def creeping_adjustment(
    distance: int,
    range_error: float,
    deflection_error: float,
    player_offset_x: float,
    player_offset_y: float,
) -> tuple[int, int, float, int, bool, float]:
    mortar_pos = (0.0, 0.0)
    target = (float(distance), 0.0)
    player_pos = (target[0] + player_offset_x, target[1] + player_offset_y)
    axis_to = make_creeping_axis_to(target, player_pos, mortar_pos)
    player_error_distance = elliptic_distance(
        mortar_pos, target, target, player_pos, range_error, deflection_error
    )
    danger_close = player_error_distance <= 2.0
    if danger_close:
        offset_multiplier = 1.5 if player_error_distance > 1.0 else 2.0
    else:
        offset_multiplier = 1.0

    offset_ux, offset_uy = axis_unit(target, axis_to)
    range_ux, range_uy = axis_unit(mortar_pos, target)
    range_component = offset_ux * range_ux + offset_uy * range_uy
    deflection_component = -offset_ux * range_uy + offset_uy * range_ux
    denominator = math.hypot(
        range_component / max(1.0, range_error),
        deflection_component / max(1.0, deflection_error),
    )
    offset_distance = offset_multiplier / denominator if denominator > 0.0 else 0.0
    offset_x = int(round(offset_ux * offset_distance))
    offset_y = int(round(offset_uy * offset_distance))
    return (
        offset_x,
        offset_y,
        math.hypot(offset_x, offset_y),
        heading_degrees(target, (target[0] + offset_x, target[1] + offset_y)),
        danger_close,
        offset_multiplier,
    )


def shot_lost_chance(
    perception: int,
    sensor_multiplier: float,
    spotter_distance_tiles: float,
    detection_range_multiplier: float,
) -> float:
    per = clamp(float(perception), 1.0, 10.0)
    if per <= 5.0:
        chance = 0.20 - (per - 1.0) * (0.10 / 4.0)
    else:
        chance = 0.10 - (per - 5.0) * (0.07 / 5.0)
    chance += 0.10 * spotter_distance_tiles / (1000.0 * max(0.1, detection_range_multiplier))
    chance /= sensor_multiplier
    return clamp(chance, 0.0, 0.95)


def feedback_observers_for_profile(profile: SpotterProfile) -> tuple[FeedbackObserver, ...]:
    if profile.feedback_observers:
        return profile.feedback_observers
    repeat_location_multiplier = profile.repeat_location_multiplier
    if profile.eplrs_net:
        repeat_location_multiplier *= MORTAR_EPLRS_LOCATION_MULTIPLIER
    return (
        FeedbackObserver(
            sensor_multiplier=profile.sensor_multiplier,
            detection_range_multiplier=profile.detection_range_multiplier,
            repeat_location_multiplier=repeat_location_multiplier,
            spotter_distance_tiles=profile.spotter_distance_tiles,
        ),
    )


def spotter_profiles(keys: Sequence[str]) -> list[SpotterProfile]:
    rangefinder_enhanced = soft_cap_sensor_multiplier(
        MORTAR_LASER_RANGEFINDER_SENSOR_MULTIPLIER * 1.4
    )
    rangefinder_nvg_ir = soft_cap_sensor_multiplier(
        MORTAR_LASER_RANGEFINDER_SENSOR_MULTIPLIER * 1.25 * 1.2
    )
    rangefinder_full_sensors = soft_cap_sensor_multiplier(
        MORTAR_LASER_RANGEFINDER_SENSOR_MULTIPLIER * 1.4 * 1.25 * 1.2
    )
    drone_sensor = soft_cap_sensor_multiplier(MORTAR_DRONE_SPOTTER_SENSOR_MULTIPLIER)
    rangefinder_eplrs_repeat = (
        MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER
        * MORTAR_EPLRS_LOCATION_MULTIPLIER
    )
    drone_eplrs_repeat = 0.5 * MORTAR_EPLRS_LOCATION_MULTIPLIER
    all_profiles = {
        "plain": SpotterProfile("plain", "plain spotter", 1.0),
        "binoculars": SpotterProfile("binoculars", "binoculars/zoom", 1.5),
        "rangefinder": SpotterProfile(
            "rangefinder",
            "laser rangefinder",
            MORTAR_LASER_RANGEFINDER_SENSOR_MULTIPLIER,
            rangefinder=True,
            repeat_location_multiplier=MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER,
        ),
        "soflam": SpotterProfile(
            "soflam",
            "SOFLAM rangefinder",
            MORTAR_LASER_RANGEFINDER_SENSOR_MULTIPLIER,
            rangefinder=True,
            repeat_location_multiplier=MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER,
        ),
        "soflam_designating": SpotterProfile(
            "soflam_designating",
            "SOFLAM active designation",
            MORTAR_LASER_RANGEFINDER_SENSOR_MULTIPLIER,
            rangefinder=True,
            repeat_location_multiplier=MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER,
        ),
        "eplrs": SpotterProfile("eplrs", "EPLRS net", 1.0, eplrs_net=True),
        "rangefinder_eplrs": SpotterProfile(
            "rangefinder_eplrs",
            "rangefinder + EPLRS",
            MORTAR_LASER_RANGEFINDER_SENSOR_MULTIPLIER,
            rangefinder=True,
            eplrs_net=True,
            repeat_location_multiplier=MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER,
        ),
        "soflam_eplrs": SpotterProfile(
            "soflam_eplrs",
            "SOFLAM + EPLRS",
            MORTAR_LASER_RANGEFINDER_SENSOR_MULTIPLIER,
            rangefinder=True,
            eplrs_net=True,
            repeat_location_multiplier=MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER,
        ),
        "rangefinder_enhanced_eplrs": SpotterProfile(
            "rangefinder_enhanced_eplrs",
            "rangefinder + enhanced vision + EPLRS",
            rangefinder_enhanced,
            rangefinder=True,
            eplrs_net=True,
            repeat_location_multiplier=MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER,
        ),
        "rangefinder_nvg_ir_eplrs": SpotterProfile(
            "rangefinder_nvg_ir_eplrs",
            "rangefinder + NVG + IR + EPLRS",
            rangefinder_nvg_ir,
            rangefinder=True,
            eplrs_net=True,
            repeat_location_multiplier=MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER,
        ),
        "rangefinder_full_sensors_eplrs": SpotterProfile(
            "rangefinder_full_sensors_eplrs",
            "rangefinder + enhanced/NVG/IR + EPLRS",
            rangefinder_full_sensors,
            rangefinder=True,
            eplrs_net=True,
            repeat_location_multiplier=MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER,
        ),
        "drone": SpotterProfile(
            "drone",
            "scout/bomber drone spotter",
            drone_sensor,
            drone=True,
            detection_range_multiplier=MORTAR_DRONE_DETECTION_RANGE_MULTIPLIER,
            spotter_distance_tiles=0.0,
        ),
        "drone_eplrs": SpotterProfile(
            "drone_eplrs",
            "drone spotter + EPLRS net",
            drone_sensor,
            eplrs_net=True,
            drone=True,
            detection_range_multiplier=MORTAR_DRONE_DETECTION_RANGE_MULTIPLIER,
            spotter_distance_tiles=0.0,
        ),
        "full_gucci": SpotterProfile(
            "full_gucci",
            "full gucci: player sensors/RF/EPLRS + drone",
            rangefinder_full_sensors,
            rangefinder=True,
            eplrs_net=True,
            repeat_location_multiplier=MORTAR_LASER_RANGEFINDER_REPEAT_LOCATION_MULTIPLIER,
            feedback_observers=(
                FeedbackObserver(
                    sensor_multiplier=rangefinder_full_sensors,
                    repeat_location_multiplier=rangefinder_eplrs_repeat,
                ),
                FeedbackObserver(
                    sensor_multiplier=drone_sensor,
                    detection_range_multiplier=MORTAR_DRONE_DETECTION_RANGE_MULTIPLIER,
                    repeat_location_multiplier=drone_eplrs_repeat,
                    spotter_distance_tiles=0.0,
                ),
            ),
        ),
    }
    return [all_profiles[key] for key in keys]


def mortar_rows(args: argparse.Namespace) -> list[MortarRow]:
    rows: list[MortarRow] = []
    profiles = spotter_profiles(parse_str_list(args.profiles))

    for profile in profiles:
        for distance in parse_int_list(args.ranges):
            for launcher_skill in parse_int_list(args.launcher_skills):
                for perception in parse_int_list(args.perceptions):
                    current_skill_multiplier = skill_accuracy_multiplier(launcher_skill)
                    current_location_cep = base_location_error(
                        perception, profile.sensor_multiplier
                    )
                    if profile.eplrs_net:
                        current_location_cep *= MORTAR_EPLRS_LOCATION_MULTIPLIER

                    for shot_index in range(args.followups + 1):
                        fixed_multiplier = fixed_accuracy_multiplier(
                            args.proficiency, args.tactical_data, shot_index == 0
                        )
                        raw_total = current_skill_multiplier * fixed_multiplier
                        total_multiplier = effective_ballistic_multiplier(raw_total)
                        ballistic_range = minimum_range_error(distance) * total_multiplier
                        ballistic_deflection = (
                            minimum_deflection_error(distance) * total_multiplier
                        )

                        location_range = current_location_cep
                        location_deflection = current_location_cep
                        if profile.rangefinder:
                            location_range *= MORTAR_LASER_RANGEFINDER_AXIS_MULTIPLIER
                        projected_range, projected_deflection = project_location_error(
                            location_range,
                            location_deflection,
                            args.location_axis_angle,
                        )
                        lost_chances: list[float] = []
                        expected_accuracy_factor = 1.0
                        expected_location_factor = 1.0
                        raw_accuracy_factors: list[float] = []
                        raw_location_factors: list[float] = []
                        for observer in feedback_observers_for_profile(profile):
                            lost = shot_lost_chance(
                                perception,
                                observer.sensor_multiplier,
                                observer.spotter_distance_tiles
                                if observer.spotter_distance_tiles is not None
                                else distance,
                                observer.detection_range_multiplier,
                            )
                            raw_accuracy = feedback_multiplier(
                                repeat_accuracy_feedback_multiplier(
                                    launcher_skill, perception, observer.sensor_multiplier
                                ),
                                args.no_wait,
                            )
                            raw_location = feedback_multiplier(
                                observer.repeat_location_multiplier,
                                args.no_wait,
                            )
                            observed_probability = 1.0 - lost
                            expected_accuracy_factor *= (
                                1.0 - observed_probability * (1.0 - raw_accuracy)
                            )
                            expected_location_factor *= (
                                1.0 - observed_probability * (1.0 - raw_location)
                            )
                            raw_accuracy_factors.append(raw_accuracy)
                            raw_location_factors.append(raw_location)
                            lost_chances.append(lost)
                        combined_lost = math.prod(lost_chances)
                        display_accuracy_factor = (
                            raw_accuracy_factors[0]
                            if len(raw_accuracy_factors) == 1
                            else expected_accuracy_factor
                        )
                        display_location_factor = (
                            raw_location_factors[0]
                            if len(raw_location_factors) == 1
                            else expected_location_factor
                        )
                        rows.append(
                            MortarRow(
                                profile=profile.label,
                                range_tiles=distance,
                                launcher_skill=launcher_skill,
                                perception=perception,
                                shot_index=shot_index,
                                fixed_multiplier=fixed_multiplier,
                                current_skill_multiplier=current_skill_multiplier,
                                total_multiplier=total_multiplier,
                                ballistic_range_tiles=ballistic_range,
                                ballistic_deflection_tiles=ballistic_deflection,
                                location_range_tiles=projected_range,
                                location_deflection_tiles=projected_deflection,
                                total_range_tiles=ballistic_range + projected_range,
                                total_deflection_tiles=ballistic_deflection
                                + projected_deflection,
                                shot_lost_pct=combined_lost * 100.0,
                                feedback_accuracy_multiplier=display_accuracy_factor,
                                feedback_location_multiplier=display_location_factor,
                            )
                        )

                        current_skill_multiplier = max(
                            1.0, current_skill_multiplier * expected_accuracy_factor
                        )
                        current_location_cep = max(
                            1.0, current_location_cep * expected_location_factor
                        )
    return rows


def creeping_rows(args: argparse.Namespace) -> list[CreepingRow]:
    shots = parse_int_list(args.creeping_shots)
    mortar_args = argparse.Namespace(**vars(args))
    mortar_args.followups = max(args.followups, max(shots, default=0))
    rows: list[CreepingRow] = []
    for row in mortar_rows(mortar_args):
        if row.shot_index not in shots:
            continue
        offset_x, offset_y, offset_distance, heading, danger_close, offset_multiplier = (
            creeping_adjustment(
                row.range_tiles,
                row.total_range_tiles,
                row.total_deflection_tiles,
                args.player_offset_x,
                args.player_offset_y,
            )
        )
        rows.append(
            CreepingRow(
                profile=row.profile,
                range_tiles=row.range_tiles,
                launcher_skill=row.launcher_skill,
                perception=row.perception,
                shot_index=row.shot_index,
                total_range_tiles=row.total_range_tiles,
                total_deflection_tiles=row.total_deflection_tiles,
                player_offset_x_tiles=args.player_offset_x,
                player_offset_y_tiles=args.player_offset_y,
                creep_offset_x_tiles=offset_x,
                creep_offset_y_tiles=offset_y,
                creep_distance_tiles=offset_distance,
                heading_degrees=heading,
                danger_close=danger_close,
                offset_multiplier=offset_multiplier,
            )
        )
    return rows


def fpv_max_range_tiles(drone_type: str) -> int:
    if drone_type == "scout":
        return 15000
    if drone_type == "baba_yaga":
        return 20000
    return 7000


def fpv_battery_seconds(drone_type: str) -> int:
    if drone_type == "scout":
        return 18 * 60
    if drone_type == "baba_yaga":
        return 24 * 60
    return 6 * 60


def fpv_cruise_speed_tiles_per_hour(drone_type: str) -> float:
    if drone_type == "baba_yaga":
        return 70000.0
    return 150000.0


def fpv_launch_delay_seconds(driving_skill: int, drone_type: str) -> int:
    base_delay = max(0, 30 - 2 * driving_skill)
    return base_delay * 2 if drone_type == "baba_yaga" else base_delay


def fpv_light_multiplier(light: str) -> float:
    if light == "dark":
        return 4.0
    if light == "dim":
        return 2.0
    return 1.0


def drone_rows(args: argparse.Namespace) -> list[DroneRow]:
    rows: list[DroneRow] = []
    drone_types = parse_str_list(args.drone_types)
    distances = parse_int_list(args.drone_distances)
    driving_skills = parse_int_list(args.driving_skills)
    lights = parse_str_list(args.lights)
    perception = args.drone_spotter_perception

    for drone_type in drone_types:
        for distance in distances:
            for driving_skill in driving_skills:
                max_range = fpv_max_range_tiles(drone_type)
                cruise_seconds = max(
                    1, math.ceil(distance * 3600.0 / fpv_cruise_speed_tiles_per_hour(drone_type))
                )
                outbound_seconds = cruise_seconds + fpv_launch_delay_seconds(
                    driving_skill, drone_type
                )
                return_seconds = cruise_seconds
                battery_seconds = fpv_battery_seconds(drone_type)
                cruise_budget = (
                    battery_seconds
                    - outbound_seconds
                    - return_seconds
                    - FPV_RESERVE_SECONDS
                )
                launchable = distance <= max_range and cruise_budget > 0
                station_seconds = (
                    max(1, math.floor(cruise_budget / FPV_STATION_BATTERY_RATE))
                    if launchable
                    else 0
                )
                one_way_station_seconds = (
                    max(
                        1,
                        math.floor(
                            (battery_seconds - outbound_seconds)
                            / FPV_STATION_BATTERY_RATE
                        ),
                    )
                    if distance <= max_range and battery_seconds > outbound_seconds
                    else 0
                )

                can_attack = drone_type in ("suicide", "military_suicide")
                can_scout = drone_type in ("scout", "baba_yaga")
                can_drop = drone_type == "baba_yaga"
                scout_delay: int | str = max(1, 20 - driving_skill) if can_scout else ""
                attack_time: int | str = max(5, 40 - driving_skill * 2) if can_attack else ""
                drop_cep: float | str = (
                    max(0.0, 4.0 - driving_skill / 3.0) if can_drop else ""
                )
                drop_delay = (
                    f"{max(1, 15 - driving_skill)}-{max(1, 15 - driving_skill) + 5}"
                    if can_drop
                    else ""
                )

                mortar_loc: float | str = ""
                mortar_lost: float | str = ""
                if can_scout:
                    sensor = soft_cap_sensor_multiplier(MORTAR_DRONE_SPOTTER_SENSOR_MULTIPLIER)
                    mortar_loc = base_location_error(perception, sensor)
                    mortar_lost = (
                        shot_lost_chance(
                            perception,
                            sensor,
                            0.0,
                            MORTAR_DRONE_DETECTION_RANGE_MULTIPLIER,
                        )
                        * 100.0
                    )

                for light in lights:
                    light_mult = fpv_light_multiplier(light)
                    fixed_cep: float | str = ""
                    creature_cep: float | str = ""
                    if can_attack:
                        base_cep = max(1.0, 15.0 - driving_skill)
                        fixed_cep = base_cep * 0.4 * light_mult
                        creature_cep = base_cep * light_mult

                    rows.append(
                        DroneRow(
                            drone_type=drone_type,
                            distance_tiles=distance,
                            driving_skill=driving_skill,
                            light=light,
                            launchable="yes" if launchable else "no",
                            eta_s=outbound_seconds if launchable else 0,
                            station_s=station_seconds,
                            return_s=return_seconds if launchable else 0,
                            one_way_station_s=one_way_station_seconds,
                            scout_delay_s=scout_delay,
                            attack_fixed_cep_tiles=fixed_cep,
                            attack_creature_cep_tiles=creature_cep,
                            attack_time_s=attack_time,
                            drop_cep_tiles=drop_cep,
                            drop_delay_s=drop_delay,
                            mortar_spotter_location_cep_tiles=mortar_loc,
                            mortar_spotter_lost_pct=mortar_lost,
                        )
                    )
    return rows


def stack_accuracy_rows(args: argparse.Namespace) -> list[SimpleNamespace]:
    rows: list[SimpleNamespace] = []
    profiles = spotter_profiles(parse_str_list(args.stack_profiles))
    rounds = selected_rounds(args.rounds)
    crew_counts = parse_int_list(args.crew_counts)
    secondary_skills = parse_int_list(args.secondary_skills)

    for mortar in selected_mortars(args.mortars):
        for round_spec in compatible_rounds_for_mortar(rounds, mortar):
            max_range = round_max_range(round_spec)
            for distance in parse_int_list(args.ranges):
                if distance > max_range:
                    continue
                accuracy_distance = round_accuracy_distance(mortar, round_spec, distance)
                for primary_skill in parse_int_list(args.launcher_skills):
                    for crew_count in crew_counts:
                        if crew_count > mortar.max_secondary_crew:
                            continue
                        skill_options = [0] if crew_count == 0 else secondary_skills
                        for secondary_skill in skill_options:
                            effective_skill = crew_effective_launcher_skill(
                                primary_skill,
                                secondary_skills_for_count(crew_count, secondary_skill),
                            )
                            for perception in parse_int_list(args.perceptions):
                                for profile in profiles:
                                    sensor = effective_profile_sensor(profile, distance)
                                    current_skill_multiplier = skill_accuracy_multiplier(effective_skill)
                                    current_location_cep = base_location_error(perception, sensor)
                                    if profile.eplrs_net:
                                        current_location_cep *= MORTAR_EPLRS_LOCATION_MULTIPLIER
                                    repeat_location = effective_repeat_location_multiplier(
                                        profile, distance
                                    )

                                    for shot_index in range(args.followups + 1):
                                        fixed_multiplier = fixed_accuracy_multiplier(
                                            args.proficiency, args.tactical_data, shot_index == 0
                                        )
                                        total_multiplier = effective_ballistic_multiplier(
                                            current_skill_multiplier * fixed_multiplier
                                        )
                                        ballistic_range = (
                                            minimum_range_error(accuracy_distance)
                                            * total_multiplier
                                        )
                                        ballistic_deflection = (
                                            minimum_deflection_error(
                                                accuracy_distance, mortar.deflection_error_mils
                                            )
                                            * total_multiplier
                                        )
                                        location_range = current_location_cep
                                        location_deflection = current_location_cep
                                        if profile.rangefinder and distance <= MORTAR_LASER_RANGEFINDER_RANGE:
                                            location_range *= MORTAR_LASER_RANGEFINDER_AXIS_MULTIPLIER
                                        projected_range, projected_deflection = project_location_error(
                                            location_range,
                                            location_deflection,
                                            args.location_axis_angle,
                                        )
                                        lost = shot_lost_chance(
                                            perception,
                                            sensor,
                                            0.0 if profile.drone else distance,
                                            profile.detection_range_multiplier,
                                        )
                                        feedback_accuracy = feedback_multiplier(
                                            repeat_accuracy_feedback_multiplier(
                                                int(round(effective_skill)), perception, sensor
                                            ),
                                            args.no_wait,
                                        )
                                        feedback_location = feedback_multiplier(
                                            repeat_location, args.no_wait
                                        )
                                        rows.append(
                                            ns(
                                                mortar=mortar.label,
                                                round=round_spec.label,
                                                guidance=round_spec.guidance,
                                                range_tiles=distance,
                                                max_range_tiles=max_range,
                                                crew=crew_count,
                                                secondary_skill=secondary_skill if crew_count else "",
                                                effective_skill=effective_skill,
                                                fire_delay_s=crew_adjusted_fire_delay(mortar, crew_count),
                                                flight_time_s=expected_round_flight_seconds(
                                                    mortar, round_spec, distance
                                                ),
                                                profile=profile.label,
                                                perception=perception,
                                                shot_index=shot_index,
                                                total_range_tiles=ballistic_range + projected_range,
                                                total_deflection_tiles=ballistic_deflection + projected_deflection,
                                                shot_lost_pct=lost * 100.0,
                                                feedback_accuracy_multiplier=feedback_accuracy,
                                                feedback_location_multiplier=feedback_location,
                                            )
                                        )
                                        observed_probability = 1.0 - lost
                                        current_skill_multiplier = max(
                                            1.0,
                                            current_skill_multiplier
                                            * (1.0 - observed_probability * (1.0 - feedback_accuracy)),
                                        )
                                        current_location_cep = max(
                                            1.0,
                                            current_location_cep
                                            * (1.0 - observed_probability * (1.0 - feedback_location)),
                                        )
    return rows


def guided_rows(args: argparse.Namespace) -> list[SimpleNamespace]:
    rows: list[SimpleNamespace] = []
    guided_rounds = [
        ROUND_SPECS["81mm_acerm"],
        ROUND_SPECS["81mm_oksi_m821"],
        ROUND_SPECS["81mm_oksi_m889"],
    ]
    scenarios = (
        ("no designation", "none", False, True, 0, False),
        ("SOFLAM tile", "soflam", True, True, 0, False),
        ("SOFLAM slow mover", "soflam", True, False, 50, True),
        ("SOFLAM fast mover", "soflam", True, False, 120, True),
        ("drone fixed tile", "drone", True, True, 0, False),
        ("drone moving target disallowed", "drone", False, False, 50, True),
    )
    profiles = {
        "none": SpotterProfile("none", "no live designator", 1.0),
        "soflam": spotter_profiles(["soflam_designating"])[0],
        "drone": spotter_profiles(["drone"])[0],
    }
    mortar = MORTAR_SPECS["m252"]
    for round_spec in guided_rounds:
        for distance in parse_int_list(args.guided_ranges):
            if distance > round_max_range(round_spec):
                continue
            accuracy_distance = round_accuracy_distance(mortar, round_spec, distance)
            for primary_skill in parse_int_list(args.guided_launcher_skills):
                for crew_count in parse_int_list(args.crew_counts):
                    if crew_count > mortar.max_secondary_crew:
                        continue
                    effective_skill = crew_effective_launcher_skill(
                        primary_skill,
                        secondary_skills_for_count(crew_count, args.guided_secondary_skill),
                    )
                    for label, source, active, tile, target_speed, moving in scenarios:
                        if round_spec.guidance == "acerm" and source == "drone":
                            continue
                        if round_spec.guidance == "oksi" and source == "soflam":
                            continue
                        if round_spec.guidance == "oksi" and not tile:
                            applies = False
                        else:
                            applies = active and (
                                (round_spec.guidance == "acerm" and source == "soflam")
                                or (round_spec.guidance == "oksi" and source == "drone")
                            )
                        profile = profiles[source]
                        sensor = effective_profile_sensor(profile, distance)
                        location_cep = base_location_error(args.guided_perception, sensor)
                        if profile.rangefinder and distance <= MORTAR_LASER_RANGEFINDER_RANGE:
                            location_range = location_cep * MORTAR_LASER_RANGEFINDER_AXIS_MULTIPLIER
                        else:
                            location_range = location_cep
                        ballistic_multiplier = effective_ballistic_multiplier(
                            skill_accuracy_multiplier(effective_skill)
                            * fixed_accuracy_multiplier(args.proficiency, args.tactical_data, True)
                        )
                        base_range = (
                            minimum_range_error(accuracy_distance)
                            * ballistic_multiplier
                            + location_range
                        )
                        base_deflection = (
                            minimum_deflection_error(
                                accuracy_distance, mortar.deflection_error_mils
                            )
                            * ballistic_multiplier
                            + location_cep
                        )
                        success_probability = (
                            designation_success_probability(
                                args.guided_perception,
                                args.guided_dexterity,
                                target_speed,
                                moving,
                                tile,
                            )
                            if applies
                            else 0.0
                        )
                        guided_range = guided_axis_error(base_range)
                        guided_deflection = guided_axis_error(base_deflection)
                        expected_range = (
                            success_probability * guided_range
                            + (1.0 - success_probability) * base_range
                        )
                        expected_deflection = (
                            success_probability * guided_deflection
                            + (1.0 - success_probability) * base_deflection
                        )
                        rows.append(
                            ns(
                                round=round_spec.label,
                                range_tiles=distance,
                                scenario=label,
                                requirement=round_spec.requirement,
                                crew=crew_count,
                                effective_skill=effective_skill,
                                designation_success_pct=success_probability * 100.0,
                                base_range_tiles=base_range,
                                base_deflection_tiles=base_deflection,
                                expected_guided_range_tiles=expected_range,
                                expected_guided_deflection_tiles=expected_deflection,
                                range_reduction_pct=(1.0 - expected_range / base_range) * 100.0,
                                deflection_reduction_pct=(1.0 - expected_deflection / base_deflection) * 100.0,
                            )
                        )
    return rows


def crew_rows(args: argparse.Namespace) -> list[SimpleNamespace]:
    rows: list[SimpleNamespace] = []
    for mortar in selected_mortars(args.mortars):
        for primary_skill in parse_int_list(args.launcher_skills):
            for crew_count in range(mortar.max_secondary_crew + 1):
                skill_options = [0] if crew_count == 0 else parse_int_list(args.secondary_skills)
                for secondary_skill in skill_options:
                    effective = crew_effective_launcher_skill(
                        primary_skill,
                        secondary_skills_for_count(crew_count, secondary_skill),
                    )
                    rows.append(
                        ns(
                            mortar=mortar.label,
                            primary_skill=primary_skill,
                            crew=crew_count,
                            secondary_skill=secondary_skill if crew_count else "",
                            effective_skill=effective,
                            skill_multiplier=skill_accuracy_multiplier(effective),
                            fire_delay_s=crew_adjusted_fire_delay(mortar, crew_count),
                            rate_multiplier=mortar.fire_delay_s
                            / crew_adjusted_fire_delay(mortar, crew_count),
                            max_secondary_crew=mortar.max_secondary_crew,
                        )
                    )
    return rows


def flight_rows(args: argparse.Namespace) -> list[SimpleNamespace]:
    rows: list[SimpleNamespace] = []
    for round_spec in selected_rounds(args.rounds):
        mortar = MORTAR_SPECS[round_spec.mortar_key]
        for distance in parse_int_list(args.flight_ranges):
            max_range = round_max_range(round_spec)
            in_range = distance <= max_range
            rows.append(
                ns(
                    mortar=mortar.label,
                    round=round_spec.label,
                    guidance=round_spec.guidance,
                    range_tiles=distance,
                    max_range_tiles=max_range,
                    in_range="yes" if in_range else "no",
                    expected_flight_s=expected_round_flight_seconds(
                        mortar, round_spec, min(distance, max_range)
                    )
                    if in_range
                    else "",
                    single_gunner_fire_delay_s=mortar.fire_delay_s,
                    max_crew_fire_delay_s=crew_adjusted_fire_delay(
                        mortar, mortar.max_secondary_crew
                    ),
                    requirement=round_spec.requirement,
                )
            )
    return rows


def equipment_rows(args: argparse.Namespace) -> list[SimpleNamespace]:
    rows: list[SimpleNamespace] = []
    profile_keys = (
        "plain,binoculars,rangefinder,soflam,soflam_designating,eplrs,"
        "rangefinder_eplrs,soflam_eplrs,drone,drone_eplrs,full_gucci"
    )
    for profile in spotter_profiles(parse_str_list(profile_keys)):
        for distance in parse_int_list(args.equipment_ranges):
            sensor = effective_profile_sensor(profile, distance)
            rows.append(
                ns(
                    profile=profile.label,
                    range_tiles=distance,
                    sensor_multiplier=sensor,
                    rangefinder_axis="yes"
                    if profile.rangefinder and distance <= MORTAR_LASER_RANGEFINDER_RANGE
                    else "no",
                    eplrs="yes" if profile.eplrs_net else "no",
                    drone="yes" if profile.drone or profile.feedback_observers else "no",
                    location_cep_p6_tiles=base_location_error(6, sensor),
                    location_cep_p8_tiles=base_location_error(8, sensor),
                    repeat_location_multiplier=effective_repeat_location_multiplier(
                        profile, distance
                    ),
                    notes=(
                        "SOFLAM burns 5 medium battery charges / 10 s while designating"
                        if "SOFLAM" in profile.label
                        else "drone can designate fixed coordinates"
                        if profile.drone
                        else ""
                    ),
                )
            )
    return rows


def designation_rows(args: argparse.Namespace) -> list[SimpleNamespace]:
    rows: list[SimpleNamespace] = []
    capabilities = (
        ("SOFLAM handheld", "ACERM", "tile/creature", "line of sight every turn", "5/10 s", "yes"),
        ("SOFLAM mounted", "ACERM", "tile only", "mountable support + LOS", "5/10 s", "yes"),
        ("scout drone", "OKSI mortar", "tile only", "scout feed visible tile", "", "yes"),
        ("baba yaga scout feed", "OKSI mortar", "tile only", "scout feed visible tile", "", "yes"),
        ("scout drone", "other mortar", "tile only", "optional drone target", "", "yes"),
        ("baba yaga scout feed", "payload drop", "tile only", "payload-ready bomber", "", "yes"),
        ("scout drone", "FPV attack", "fixed point", "optional fixed target", "", "yes"),
        ("no live designator", "guided mortar", "none", "falls back unguided / invalid OKSI", "", "no"),
    )
    for name, use, target, condition, burn, valid in capabilities:
        for range_tiles in parse_int_list(args.designation_ranges):
            rows.append(
                ns(
                    designator=name,
                    use=use,
                    target=target,
                    range_tiles=range_tiles,
                    condition=condition,
                    battery_burn=burn,
                    valid=valid,
                    turn_based_activity="yes" if name.startswith("SOFLAM") else "",
                )
            )
    return rows


def stack_creeping_rows(args: argparse.Namespace) -> list[SimpleNamespace]:
    rows: list[SimpleNamespace] = []
    for row in stack_accuracy_rows(args):
        if row.shot_index not in parse_int_list(args.creeping_shots):
            continue
        if "HE" not in row.round:
            continue
        offset_x, offset_y, offset_distance, heading, danger_close, offset_multiplier = (
            creeping_adjustment(
                row.range_tiles,
                row.total_range_tiles,
                row.total_deflection_tiles,
                args.player_offset_x,
                args.player_offset_y,
            )
        )
        rows.append(
            ns(
                mortar=row.mortar,
                round=row.round,
                range_tiles=row.range_tiles,
                crew=row.crew,
                profile=row.profile,
                shot_index=row.shot_index,
                total_range_tiles=row.total_range_tiles,
                total_deflection_tiles=row.total_deflection_tiles,
                player_offset_x_tiles=args.player_offset_x,
                creep_offset_x_tiles=offset_x,
                creep_offset_y_tiles=offset_y,
                creep_distance_tiles=offset_distance,
                heading_degrees=heading,
                danger_close=danger_close,
                offset_multiplier=offset_multiplier,
            )
        )
    return rows


def scenario_rows(args: argparse.Namespace) -> list[SimpleNamespace]:
    rows: list[SimpleNamespace] = []
    scenarios = (
        ("60mm baseline", "m224", "60mm_he_m720", "plain", 0, 0, 1000),
        ("60mm assistant", "m224", "60mm_he_m768", "rangefinder", 1, 8, 2000),
        ("81mm baseline", "m252", "81mm_he_m821", "plain", 0, 0, 3000),
        ("81mm one crew", "m252", "81mm_he_m889", "soflam", 1, 8, 3000),
        ("81mm full crew", "m252", "81mm_he_m821", "soflam_eplrs", 2, 8, 5000),
        ("OKSI drone", "m252", "81mm_oksi_m821", "drone", 2, 8, 5000),
        ("ACERM close", "m252", "81mm_acerm", "soflam_designating", 2, 8, 5900),
        ("ACERM glide", "m252", "81mm_acerm", "soflam_designating", 2, 8, 12000),
        ("ACERM max", "m252", "81mm_acerm", "soflam_designating", 2, 8, 20000),
    )
    for name, mortar_key, round_key, profile_key, crew, secondary, distance in scenarios:
        mortar = MORTAR_SPECS[mortar_key]
        round_spec = ROUND_SPECS[round_key]
        profile = spotter_profiles([profile_key])[0]
        accuracy_distance = round_accuracy_distance(mortar, round_spec, distance)
        for primary_skill in parse_int_list(args.scenario_launcher_skills):
            for perception in parse_int_list(args.scenario_perceptions):
                effective = crew_effective_launcher_skill(
                    primary_skill, secondary_skills_for_count(crew, secondary)
                )
                sensor = effective_profile_sensor(profile, distance)
                fixed = fixed_accuracy_multiplier(args.proficiency, args.tactical_data, True)
                total_multiplier = effective_ballistic_multiplier(
                    skill_accuracy_multiplier(effective) * fixed
                )
                location = base_location_error(perception, sensor)
                location_range = location
                if profile.rangefinder and distance <= MORTAR_LASER_RANGEFINDER_RANGE:
                    location_range *= MORTAR_LASER_RANGEFINDER_AXIS_MULTIPLIER
                base_range = (
                    minimum_range_error(accuracy_distance) * total_multiplier
                    + location_range
                )
                base_deflection = (
                    minimum_deflection_error(
                        accuracy_distance, mortar.deflection_error_mils
                    )
                    * total_multiplier
                    + location
                )
                guided = round_spec.guidance in ("acerm", "oksi")
                final_range = guided_axis_error(base_range) if guided else base_range
                final_deflection = guided_axis_error(base_deflection) if guided else base_deflection
                rows.append(
                    ns(
                        scenario=name,
                        mortar=mortar.label,
                        round=round_spec.label,
                        range_tiles=distance,
                        primary_skill=primary_skill,
                        perception=perception,
                        crew=crew,
                        effective_skill=effective,
                        fire_delay_s=crew_adjusted_fire_delay(mortar, crew),
                        flight_time_s=expected_round_flight_seconds(mortar, round_spec, distance),
                        profile=profile.label,
                        base_range_tiles=base_range,
                        base_deflection_tiles=base_deflection,
                        final_range_tiles=final_range,
                        final_deflection_tiles=final_deflection,
                    )
                )
    return rows


def cache_rows(args: argparse.Namespace) -> list[SimpleNamespace]:
    rows: list[SimpleNamespace] = []
    entries = (
        ("ACERM cache", "ammo_can_81mm_acerm_full", "100%", "ACERM reserve can", ""),
        ("ACERM cache", "ammo_tube_81mm_acerm_full", "60%, 1-2", "ready ACERM tubes", ""),
        ("ACERM cache", "AN/PEQ-1C SOFLAM", "100%, 80-250 charges", "laser designation/rangefinding", "5 charges / 10 s designation"),
        ("ACERM cache", "OKSI 81mm PGK", "80%, 1-3", "converts M821A2/M889A1 HE", "requires mortar proficiency"),
        ("ACERM cache", "medium battery cell", "70%, 1-3", "SOFLAM power", ""),
        ("OKSI kit", "M821A2 81mm HE -> OKSI M821A2", "manual use", "guided fixed-tile round", "drone designation required"),
        ("OKSI kit", "M889A1 81mm HE -> OKSI M889A1", "manual use", "guided fixed-tile round", "drone designation required"),
        ("SOFLAM", "medium battery", "magazine well", "field-rugged designator", "also acts as binocular/rangefinder"),
        ("laser rangefinder", "light/ultralight battery", "magazine well", "rangefinder only", "2000 tile rangefinder regime"),
        ("M252 crew", "primary + up to 2 assistants", "activity state", "half fire delay, then -2 s per assistant", "primary owns mortar assignment"),
        ("M224 crew", "primary + up to 1 assistant", "activity state", "half fire delay, then -2 s with assistant", "primary owns mortar assignment"),
        ("drone designation", "scout or baba yaga", "after scout feed", "fixed tile designation", "OKSI/alternate mortar/FPV/drop target"),
    )
    for category, item, availability, role, notes in entries:
        for branch_state in ("81mm branch", "stack-drones merged"):
            rows.append(
                ns(
                    category=category,
                    item=item,
                    availability=availability,
                    role=role,
                    notes=notes,
                    branch_state=branch_state,
                )
            )
    return rows


HUMANIZED_TILE_BUCKETS = (5, 10, 15, 20, 30, 40, 50, 70, 100, 150, 200)

HUMANIZED_FIELDS = {
    "attack_creature_cep_tiles",
    "attack_fixed_cep_tiles",
    "ballistic_deflection_tiles",
    "ballistic_range_tiles",
    "base_deflection_tiles",
    "base_range_tiles",
    "drop_cep_tiles",
    "expected_guided_deflection_tiles",
    "expected_guided_range_tiles",
    "final_deflection_tiles",
    "final_range_tiles",
    "location_deflection_tiles",
    "location_range_tiles",
    "mortar_spotter_location_cep_tiles",
    "total_deflection_tiles",
    "total_range_tiles",
}


def humanized_tiles(value: object) -> object:
    if value == "" or value is None:
        return value
    numeric = float(value)
    if numeric <= 0.0:
        return 0
    for bucket in HUMANIZED_TILE_BUCKETS:
        if numeric <= bucket:
            return bucket
    return int(math.ceil(numeric / 300.0) * 300)


def report_value(field: str, value: object) -> object:
    if field in HUMANIZED_FIELDS:
        return humanized_tiles(value)
    return value


def format_value(value: object) -> str:
    if isinstance(value, float):
        return f"{value:.2f}"
    return str(value)


def write_csv(rows: Iterable[object], fieldnames: Sequence[str], out) -> None:
    writer = csv.DictWriter(out, fieldnames=fieldnames, lineterminator="\n")
    writer.writeheader()
    for row in rows:
        writer.writerow({field: report_value(field, getattr(row, field)) for field in fieldnames})


def write_markdown(
    title: str, rows: Iterable[object], fieldnames: Sequence[str], out
) -> None:
    print(f"## {title}", file=out)
    print("| " + " | ".join(fieldnames) + " |", file=out)
    print("| " + " | ".join("---" for _ in fieldnames) + " |", file=out)
    for row in rows:
        print(
            "| "
            + " | ".join(format_value(report_value(field, getattr(row, field))) for field in fieldnames)
            + " |",
            file=out,
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--table",
        choices=(
            "all",
            "mortar",
            "drone",
            "creeping",
            "stack_accuracy",
            "guided",
            "crew",
            "flight",
            "equipment",
            "designation",
            "stack_creeping",
            "scenario",
            "cache",
            "stack_report",
        ),
        default="all",
        help="which table set to print",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "csv"),
        default="markdown",
        help="output format; csv prints one table at a time",
    )
    parser.add_argument("--ranges", default="500,1000,2000,3000,5000,5900,10000,20000")
    parser.add_argument("--launcher-skills", default="4,6,8,10")
    parser.add_argument("--perceptions", default="4,6,8,10")
    parser.add_argument("--mortars", default="m224,m252")
    parser.add_argument(
        "--rounds",
        default=(
            "60mm_he_m720,60mm_he_m768,81mm_he_m821,81mm_he_m889,"
            "81mm_oksi_m821,81mm_oksi_m889,81mm_acerm,81mm_smoke_m819,81mm_illum_m853"
        ),
    )
    parser.add_argument("--crew-counts", default="0,1,2")
    parser.add_argument("--secondary-skills", default="4,8,10")
    parser.add_argument(
        "--stack-profiles",
        default="plain,soflam,soflam_eplrs,drone,drone_eplrs,full_gucci",
    )
    parser.add_argument(
        "--profiles",
        default="plain,rangefinder,rangefinder_eplrs,drone,drone_eplrs",
        help=(
            "comma-separated mortar profiles: plain, binoculars, rangefinder, "
            "soflam, soflam_designating, eplrs, rangefinder_eplrs, soflam_eplrs, "
            "rangefinder_enhanced_eplrs, "
            "rangefinder_nvg_ir_eplrs, rangefinder_full_sensors_eplrs, "
            "drone, drone_eplrs, full_gucci"
        ),
    )
    parser.add_argument(
        "--followups",
        type=int,
        default=2,
        help="number of expected observed follow-up corrections to project",
    )
    parser.add_argument(
        "--no-wait",
        action="store_true",
        help="model fire-for-effect/no-wait feedback scaling",
    )
    parser.add_argument(
        "--no-proficiency",
        dest="proficiency",
        action="store_false",
        help="model a gunner without prof_mortar_operation",
    )
    parser.set_defaults(proficiency=True)
    parser.add_argument(
        "--no-tactical-data",
        dest="tactical_data",
        action="store_false",
        help="model a gunner without a fire-control tablet/tactical data system",
    )
    parser.set_defaults(tactical_data=True)
    parser.add_argument(
        "--location-axis-angle",
        type=float,
        default=0.0,
        help="degrees between ballistic axis and spotter location axis",
    )
    parser.add_argument(
        "--creeping-shots",
        default="1,2,3",
        help="comma-separated shot indexes to include in the creeping table",
    )
    parser.add_argument(
        "--player-offset-x",
        type=float,
        default=-100.0,
        help="player X offset from target, in tiles, for creeping adjustment",
    )
    parser.add_argument(
        "--player-offset-y",
        type=float,
        default=0.0,
        help="player Y offset from target, in tiles, for creeping adjustment",
    )
    parser.add_argument(
        "--drone-types",
        default="suicide,military_suicide,scout,baba_yaga",
        help="comma-separated drone types",
    )
    parser.add_argument("--drone-distances", default="500,1000,3000,7000,15000,20000")
    parser.add_argument("--driving-skills", default="0,4,8,10")
    parser.add_argument("--lights", default="lit,dark")
    parser.add_argument(
        "--drone-spotter-perception",
        type=int,
        default=8,
        help="operator perception used for drone-mortar spotting columns",
    )
    parser.add_argument("--guided-ranges", default="100,500,1000,2000,5900,10000,15000,20000")
    parser.add_argument("--guided-launcher-skills", default="4,6,8,10")
    parser.add_argument("--guided-secondary-skill", type=int, default=8)
    parser.add_argument("--guided-perception", type=int, default=8)
    parser.add_argument("--guided-dexterity", type=int, default=8)
    parser.add_argument("--flight-ranges", default="100,500,1000,2000,3500,5900,10000,15000,20000")
    parser.add_argument("--equipment-ranges", default="500,1500,2500,5900")
    parser.add_argument("--designation-ranges", default="500,2000,5900,12000")
    parser.add_argument("--scenario-launcher-skills", default="4,8,10")
    parser.add_argument("--scenario-perceptions", default="6,8,10")
    return parser


MORTAR_FIELDS = (
    "profile",
    "range_tiles",
    "launcher_skill",
    "perception",
    "shot_index",
    "fixed_multiplier",
    "current_skill_multiplier",
    "total_multiplier",
    "ballistic_range_tiles",
    "ballistic_deflection_tiles",
    "location_range_tiles",
    "location_deflection_tiles",
    "total_range_tiles",
    "total_deflection_tiles",
    "shot_lost_pct",
    "feedback_accuracy_multiplier",
    "feedback_location_multiplier",
)

DRONE_FIELDS = (
    "drone_type",
    "distance_tiles",
    "driving_skill",
    "light",
    "launchable",
    "eta_s",
    "station_s",
    "return_s",
    "one_way_station_s",
    "scout_delay_s",
    "attack_fixed_cep_tiles",
    "attack_creature_cep_tiles",
    "attack_time_s",
    "drop_cep_tiles",
    "drop_delay_s",
    "mortar_spotter_location_cep_tiles",
    "mortar_spotter_lost_pct",
)

CREEPING_FIELDS = (
    "profile",
    "range_tiles",
    "launcher_skill",
    "perception",
    "shot_index",
    "total_range_tiles",
    "total_deflection_tiles",
    "player_offset_x_tiles",
    "player_offset_y_tiles",
    "creep_offset_x_tiles",
    "creep_offset_y_tiles",
    "creep_distance_tiles",
    "heading_degrees",
    "danger_close",
    "offset_multiplier",
)

STACK_ACCURACY_FIELDS = (
    "mortar",
    "round",
    "guidance",
    "range_tiles",
    "max_range_tiles",
    "crew",
    "secondary_skill",
    "effective_skill",
    "fire_delay_s",
    "flight_time_s",
    "profile",
    "perception",
    "shot_index",
    "total_range_tiles",
    "total_deflection_tiles",
    "shot_lost_pct",
    "feedback_accuracy_multiplier",
    "feedback_location_multiplier",
)

GUIDED_FIELDS = (
    "round",
    "range_tiles",
    "scenario",
    "requirement",
    "crew",
    "effective_skill",
    "designation_success_pct",
    "base_range_tiles",
    "base_deflection_tiles",
    "expected_guided_range_tiles",
    "expected_guided_deflection_tiles",
    "range_reduction_pct",
    "deflection_reduction_pct",
)

CREW_FIELDS = (
    "mortar",
    "primary_skill",
    "crew",
    "secondary_skill",
    "effective_skill",
    "skill_multiplier",
    "fire_delay_s",
    "rate_multiplier",
    "max_secondary_crew",
)

FLIGHT_FIELDS = (
    "mortar",
    "round",
    "guidance",
    "range_tiles",
    "max_range_tiles",
    "in_range",
    "expected_flight_s",
    "single_gunner_fire_delay_s",
    "max_crew_fire_delay_s",
    "requirement",
)

EQUIPMENT_FIELDS = (
    "profile",
    "range_tiles",
    "sensor_multiplier",
    "rangefinder_axis",
    "eplrs",
    "drone",
    "location_cep_p6_tiles",
    "location_cep_p8_tiles",
    "repeat_location_multiplier",
    "notes",
)

DESIGNATION_FIELDS = (
    "designator",
    "use",
    "target",
    "range_tiles",
    "condition",
    "battery_burn",
    "valid",
    "turn_based_activity",
)

STACK_CREEPING_FIELDS = (
    "mortar",
    "round",
    "range_tiles",
    "crew",
    "profile",
    "shot_index",
    "total_range_tiles",
    "total_deflection_tiles",
    "player_offset_x_tiles",
    "creep_offset_x_tiles",
    "creep_offset_y_tiles",
    "creep_distance_tiles",
    "heading_degrees",
    "danger_close",
    "offset_multiplier",
)

SCENARIO_FIELDS = (
    "scenario",
    "mortar",
    "round",
    "range_tiles",
    "primary_skill",
    "perception",
    "crew",
    "effective_skill",
    "fire_delay_s",
    "flight_time_s",
    "profile",
    "base_range_tiles",
    "base_deflection_tiles",
    "final_range_tiles",
    "final_deflection_tiles",
)

CACHE_FIELDS = (
    "category",
    "item",
    "availability",
    "role",
    "notes",
    "branch_state",
)


STACK_TABLES = (
    ("Stack Mortar Accuracy Matrix", stack_accuracy_rows, STACK_ACCURACY_FIELDS),
    ("Guided Shot Regimes", guided_rows, GUIDED_FIELDS),
    ("Crew Skill And Rate", crew_rows, CREW_FIELDS),
    ("Round Range And Flight Envelope", flight_rows, FLIGHT_FIELDS),
    ("Equipment And Sensor State", equipment_rows, EQUIPMENT_FIELDS),
    ("Drone Support Sweep", drone_rows, DRONE_FIELDS),
    ("Designation Capability Matrix", designation_rows, DESIGNATION_FIELDS),
    ("Stack Creeping Adjustment", stack_creeping_rows, STACK_CREEPING_FIELDS),
    ("Representative Fire Missions", scenario_rows, SCENARIO_FIELDS),
    ("Cache And Added Equipment Coverage", cache_rows, CACHE_FIELDS),
)


TABLE_BUILDERS = {
    "mortar": ("Mortar Accuracy Sweep", mortar_rows, MORTAR_FIELDS),
    "drone": ("Drone Support Sweep", drone_rows, DRONE_FIELDS),
    "creeping": ("Mortar Creeping Adjustment Sweep", creeping_rows, CREEPING_FIELDS),
    "stack_accuracy": STACK_TABLES[0],
    "guided": STACK_TABLES[1],
    "crew": STACK_TABLES[2],
    "flight": STACK_TABLES[3],
    "equipment": STACK_TABLES[4],
    "designation": STACK_TABLES[6],
    "stack_creeping": STACK_TABLES[7],
    "scenario": STACK_TABLES[8],
    "cache": STACK_TABLES[9],
}


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.format == "csv" and args.table in ("all", "stack_report"):
        parser.error("--format csv requires a single concrete --table")

    if args.table == "stack_report":
        if args.format == "csv":
            parser.error("--format csv requires a single concrete --table")
        for table_index, (title, builder, fields) in enumerate(STACK_TABLES):
            if table_index:
                print(file=sys.stdout)
            write_markdown(title, builder(args), fields, sys.stdout)
        return 0

    if args.table in TABLE_BUILDERS:
        title, builder, fields = TABLE_BUILDERS[args.table]
        rows = builder(args)
        if args.format == "csv":
            write_csv(rows, fields, sys.stdout)
        else:
            write_markdown(title, rows, fields, sys.stdout)
        return 0

    if args.table in ("all", "mortar"):
        rows = mortar_rows(args)
        if args.format == "csv":
            write_csv(rows, MORTAR_FIELDS, sys.stdout)
        else:
            write_markdown("Mortar Accuracy Sweep", rows, MORTAR_FIELDS, sys.stdout)

    if args.table in ("all", "drone"):
        rows = drone_rows(args)
        if args.format == "csv":
            write_csv(rows, DRONE_FIELDS, sys.stdout)
        else:
            if args.table == "all":
                print(file=sys.stdout)
            write_markdown("Drone Support Sweep", rows, DRONE_FIELDS, sys.stdout)

    if args.table in ("all", "creeping"):
        rows = creeping_rows(args)
        if args.format == "csv":
            write_csv(rows, CREEPING_FIELDS, sys.stdout)
        else:
            if args.table == "all":
                print(file=sys.stdout)
            write_markdown("Mortar Creeping Adjustment Sweep", rows, CREEPING_FIELDS, sys.stdout)

    if args.table == "all":
        for title, builder, fields in STACK_TABLES:
            print(file=sys.stdout)
            write_markdown(title, builder(args), fields, sys.stdout)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
