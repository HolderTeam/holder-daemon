# Swagger Tutorial (Holder API)

This walkthrough uses Swagger UI to:
- Create a project
- Create two cards
- List cards in the project
- Search for a keyword in card content

Open Swagger UI:
`http://127.0.0.1:11499/docs`

## 0) Set the auth token

1. Click the "Authorize" button in Swagger UI.
2. Paste the token from the server startup log:
   - Example: `Bearer abc123...`
3. Click "Authorize", then "Close".

## 1) Create a project

In Swagger UI, open `POST /projects` and click "Try it out".

Request body:
```json
{
  "name": "My First Project",
  "root_path": "/home/me/notes"
}
```

Execute. Copy the `project_id` from the response for the next steps.

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

## 3) List cards in the project

Open `GET /cards` and click "Try it out".

Query params:
- `project_id`: `<project_id>`

Execute. You should see both cards in the response list.

## 4) Search for a keyword

Open `GET /search/cards` and click "Try it out".

Query params:
- `project_id`: `<project_id>`
- `q`: `espresso`

Execute. You should see the first card returned with a snippet.
