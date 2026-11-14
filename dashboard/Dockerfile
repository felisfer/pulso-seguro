ARG NODE_VERSION=24.19.0-alpine
ARG NGINX_VERSION=1.31.8-alpine

# ===== STAGE 1: Base =====
FROM node:${NODE_VERSION} AS base

RUN apk update && apk upgrade --no-cache


# ===== STAGE 2: Dependencies =====
FROM base AS dependencies

WORKDIR /app

COPY package.json pnpm-lock.yaml* ./

RUN corepack enable pnpm && pnpm install --frozen-lockfile


# ===== STAGE 3: Build =====
FROM base AS build

# Vite incrusta las VITE_* al compilar leyendo .env.production, que sí entra al
# contexto (está en .gitignore pero no en .dockerignore). No declares aquí ARG/ENV
# VITE_*: Vite da prioridad al entorno del proceso y, aunque estén vacías, pisan
# los valores de .env.production.

WORKDIR /app

COPY --from=dependencies /app/node_modules ./node_modules

COPY . .

RUN corepack enable pnpm && pnpm build


# ===== STAGE 4: Production =====
FROM nginx:${NGINX_VERSION} AS production

RUN apk update && apk upgrade --no-cache

RUN rm -f /etc/nginx/conf.d/default.conf

COPY nginx.conf /etc/nginx/conf.d/default.conf

COPY --from=build /app/dist/ /usr/share/nginx/html/

EXPOSE 80


# ===== STAGE 5: Development =====
FROM base AS development

ENV NODE_ENV=development

ENV PORT=5173

WORKDIR /app

COPY --from=dependencies /app/node_modules ./node_modules

COPY . .

RUN corepack enable pnpm

EXPOSE ${PORT}

CMD ["pnpm", "run", "dev", "--host"]
