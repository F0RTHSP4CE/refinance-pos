import hashlib

# cards.json content as is
users = {}

salt = ""

for username, uids in users.items():
    for uid in uids:
        # Compute SHA256 of uid + salt
        to_hash = uid.upper() + salt
        hashed = hashlib.sha256(to_hash.encode("utf-8")).hexdigest()
        print(f"{username} - {hashed}")
