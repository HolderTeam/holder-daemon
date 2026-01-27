# Swagger Tutorial (Holder API)

This walkthrough uses Swagger UI to:
- Create a project
- Create two cards
- Link the cards
- List cards in the project
- Search for a keyword in card content
- Manage trash (delete/restore/empty)

Open Swagger UI:
`http://127.0.0.1:11499/docs`

## 0) Set the auth token

1. Click the "Authorize" button in Swagger UI.
2. Paste the token from the server startup log:
   - Example: `Bearer abc123...`
3. Click "Authorize", then "Close".

## 1) Create a project

In Swagger UI, open `POST /projects` and click "Try it out".

Request body (minimal):
```json
{
  "name": "My First Project"
}
```

Execute. The server will pick a default `root_path`. Copy the `project_id` from
the response for the next steps.

## 2) Create two cards

Open `POST /cards` and click "Try it out".

Card 1:
```json
{
  "project_id": "<project_id>",
  "title": "First note",
  "content": "This card mentions espresso beans."
}
```

Execute. Repeat with Card 2:
```json
{
  "project_id": "<project_id>",
  "title": "Second note",
  "content": "This card is about filters and grinders."
}
```

Copy the `card_id` values from both responses (call them `<card_id_a>` and
`<card_id_b>`).

## 3) Link the cards

Open `POST /cards/{card_id}/links` and click "Try it out".

Path param:
- `card_id`: `<card_id_a>`

Request body:
```json
{
  "to_card_id": "<card_id_b>",
  "kind": "ref",
  "label": "See also"
}
```

Execute. Then check backlinks with `GET /cards/{card_id}/backlinks`:
- `card_id`: `<card_id_b>`

## 4) List cards in the project

Open `GET /cards` and click "Try it out".

Query params:
- `project_id`: `<project_id>`

Execute. You should see both cards in the response list.

## 5) Search for a keyword

Open `GET /search/cards` and click "Try it out".

Query params:
- `project_id`: `<project_id>`
- `q`: `espresso`

Execute. You should see the first card returned with a snippet.

## 6) Trash (delete/restore/empty)

Delete a card (moves it to trash):
Open `DELETE /cards/{card_id}` and click "Try it out".
- `card_id`: `<card_id_a>`

List trash for the project:
Open `GET /trash` and click "Try it out".
- `project_id`: `<project_id>`
- `type`: `all`

Restore the card:
Open `POST /cards/{card_id}/restore`.
- `card_id`: `<card_id_a>`

Delete an AI message (moves it to trash):
Open `DELETE /ai/messages/{message_id}` with a valid message id.

Restore the AI message:
Open `POST /ai/messages/{message_id}/restore`.

Empty trash:
Open `DELETE /trash`.
- `project_id`: `<project_id>`
- `type`: `all`
