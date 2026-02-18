"""Utility to print UID -> identifier mappings for by-identifier endpoints.

Populate `users` with:
{
    "entity_name": ["UID1", "UID2"]
}
"""

users = {}

for entity_name, uids in users.items():
    for uid in uids:
        print(f"uid={uid.upper()} identifier={entity_name}")
